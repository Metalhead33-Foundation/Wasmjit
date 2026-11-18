# Synthetic Objects {#prop_synthetic_objects}

*Host-Defined Object Forms, the GObject Precedent, and What the Component Model Already Covers*

Status: design note / implementation reference (revision 1 — 2026-11-18). This is
`EMBEDDING_SURFACE.md` §16 promoted to a note of its own: that section had grown
into a six-hundred-line study inside an implementation reference, and the
reference now keeps only the embedder-facing half. **Contains no new code.** It
decides three things — whether a host object model should exist, what shape the
embedder gives it, and how much of it the Component Model already owns — and it
records the answer as a boundary, not a milestone.

Audience: anyone designing a cross-language object layer, anyone asking whether
“C can use a Wasm GC object” is an engine gap ([`../SCOPE.md`](../SCOPE.md) §3.2),
and anyone extending the generator plan of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
§11/§14.

Follows the same split as [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) and
[`TYPE_MODEL.md`](TYPE_MODEL.md). **Appendix A** carries the study itself, at the
section numbers it had as `EMBEDDING_SURFACE.md` §16.x (so an old reference still
resolves: 16.5 → A.5); §2–§8 state the decisions and cite it rather than repeat it.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the scenario demand? |
| §3 | What does it *not* demand? |
| §4 | What is implemented today? |
| §5 | What is missing or untested (`SO1`–`SO12`)? |
| §6 | Where do we deliberately deviate (`SOD1`–`SOD6`)? |
| §7 | What is the plan, and is a milestone opened (`SO-1`–`SO-4`)? |
| §8 | What does the test survey measure? |
| Appendix A | The study: a GObject-shaped host object runtime, in full |
| §9 | References — the last section, as [§2.3](../PROPOSALS.md) requires |

## 2. What the scenario demands

### 2.1 The synthetic-object convention

A **synthetic module** is not a `Module`: it is a `(module, field)` resolver
convention whose entries are host-provided functions, memories, tables or globals,
and the guest sees ordinary imports ([`../SCOPE.md`](../SCOPE.md) §3.1,
[`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §4.6,
[`../languages/LF2_ABI_INTEROP.md`](../languages/LF2_ABI_INTEROP.md) §2).

This note names the object-side analogue. A **synthetic object** is not a
`TypeId`, not a `GcHeader` and not a `TypeRegistry` entry: it is a *convention* by
which a host-defined class is reached from Wasm through an integer, described by
a host-side descriptor.

| The convention fixes | What it is | What it must **not** be |
|---|---|---|
| the handle | one `i64`/`u64`, or `externref` where the toolchain can spell it | a pointer, a `TypeId`, a `(ref $T)` |
| the descriptor | a stable type name, a version, fields, methods | a Wasm type, a C++ RTTI name, a field offset |
| the lifetime | one owner per object graph (§3.3) | a second count beside the GC's |
| the dispatch | a host function table | a vtable, a `call_ref` |

Nothing in that table needs an engine object — the same move as the synthetic
module, which is why the answer to “is this an engine gap?” is **no**
([`../SCOPE.md`](../SCOPE.md) §3.2). GObject is the precedent for the shape, not a
dependency: it demonstrates that a C-first object system *can* exist with
registration, properties, signals and interfaces, and it equally demonstrates what
the shape costs (§2.3).

### 2.2 Three layers, often conflated

The question “can C use a typed `(ref $T)`?” mixes three decisions:

| Layer | What it decides | Who can change it |
|---|---|---|
| **Wire representation** | one pointer-sized word per reference, or a `u32`/`u64` handle | the engine/backend ABI ([`../ABI.md`](../ABI.md)); already fixed |
| **Wasm type relation** | whether the value is `(ref $T)`, `externref`, `anyref`, a resource, and whether it `match`es | the Wasm spec and the guest toolchain |
| **Object model** | creation, fields, methods, identity, inheritance, lifetime, threading | a host runtime, an embedder, or a language runtime |

A synthetic object can own the third layer. It cannot change the second for a C
guest: a C compiler that does not emit GC reference types cannot declare a typed
`(ref $T)` import, no matter what the host provides. What it *can* do is make the
**value** usable — the guest carries an opaque handle and calls functions the
runtime implements. That is a change of API, not of type system.

This is why the honest target is not “C understands a Wasm GC object”. It is “C
can use a host object through a documented handle ABI, and a GC language can use
the same object through a generated wrapper” (§3.3 separates the strengths).

**If a C compiler did gain a typed `(ref $T)` extension**, three things would
still be true: the C code would need GC rooting/barriers at every local and store;
field access would have to follow the Wasm GC layout, not a C `struct`; and the
type would be the canonical Wasm type, not a C tag. The host runtime would become
an implementation detail rather than the mechanism. That is a **LANG**/**TOOL**
project, not an engine feature.

“Everyone understands it” is likewise not a property a runtime can grant. It is
either a **shared canonical Wasm type** (structural, same `Store`) or a **shared
IDL/resource convention** (nominal, via WIT or a descriptor name). §3.3 separates
those meanings; §2.4 measures the Component Model overlap.


### 2.3 What GObject contributes, and what transfers

GObject is the closest prior art for a C-first cross-language object system. Its
type system is built on `GType`; its object model adds reference counting,
properties/`GValue`, signals, interfaces and boxed types. The
[GObject tutorial](https://docs.gtk.org/gobject/tutorial) is also the honest
warning: a class is declared through a macro trio, properties are installed by
hand, and every signal needs a marshaller.

| GObject piece | Transfers to a synthetic object? |
|---|---|
| a registered, named type with a parent | **yes** — a descriptor with a stable name and version |
| reference counting (`g_object_ref`/`unref`) | **yes** — one count per graph (§3.3) |
| interfaces (`GInterface`, `query_interface`) | **yes** — a host descriptor query, not a Wasm type |
| properties and `GValue` | **only as a dynamic feature**; a static field accessor is cheaper and is what most products need |
| signals | **expressible**, but as functions or function-typed parameters; not an engine feature |
| boxed types (`GBytes`/`GVariant`/`GArray`/`GMainLoop`) | **no new idea** — canonical records/lists/byte arrays, or the `ComponentExecutor` seam |
| the macro ergonomics | **no** — generated bindings are the modern replacement, and WIT already is one (§2.4) |

The transferable core is therefore the **dynamic, introspectable object model** —
registration, properties, signals, interface queries — not the container types or
the main loop, which the Component Model (or the executor seam) already covers.

### 2.4 The standard form that already exists

The Component Model specifies the wire contract this idea needs: **nominal
resource types**, `own`/`borrow` handles, `resource.drop` with a destructor,
`resource.rep`, canonical lift/lower, and generated bindings from WIT. The
engine's component runtime has host-created resource handles
(`newResourceHandle`/`resourceRepresentation`/`dropResourceHandle`) and validates
stale, mistyped and out-of-scope handles. That changes the redundancy answer from
“conceptually similar” to “already implemented at the wire level”.

| Capability | Component Model | A synthetic object without components | Overlap |
|---|---|---|---|
| opaque typed handle | `resource` + `own`/`borrow` | `u64`/`externref` + table tag | **redundant** |
| destructor on last release | `resource.drop` / declared destructor | `release`/`drop` calls host deleter | **redundant** |
| cross-language bindings | WIT + toolchain (`wit-bindgen`-style) | generated C/Rust/… headers | **mostly redundant**; the generator is the same kind of tool |
| records/variants/lists/strings | Canonical ABI | canonical records plus host functions | **redundant** |
| method calls | WIT functions over a resource | host function table | **redundant as wire protocol** |
| field get/set | WIT functions over a resource | typed accessors | **redundant as wire protocol**; useful as generated ergonomics |
| dynamic properties/`GValue` | not specified | host registry + `get/set_property(name)` | **not redundant** |
| signals/events | not specified; streams/futures are a different abstraction | host signal table | **not redundant**, but expressible as functions |
| interfaces/inheritance | resources are nominal and have no subtyping | host `query_interface`/`is_a` | **not redundant**; still host-side, not Wasm types |
| runtime reflection | component types are structural; no object introspection | host type descriptors | **not redundant** |
| cross-instance identity | `own` moves, `borrow` is scoped; each instance has its own table | host table | **partly redundant**; the host must define its scope |
| C guest declares `(ref $T)` | no; C gets a resource handle and functions | no; C gets a handle and functions | **neither solves the type-level question**; both solve the practical one |

“C gets a resource handle” above means C **compiled to a component** with
generated bindings; a plain core C module must go through a canonical lift/lower
adapter or a synthetic module, which is the same handle-plus-functions shape.

**Verdict.** If the goal is “a C guest can create, use and release a host object
through a typed interface”, the Component Model already owns the contract and the
host runtime is an **implementation behind a WIT resource**, not a second ABI. If
the goal is a **dynamic object model** (properties by name, signals, reflection,
interface queries, a class hierarchy), the Component Model does not standardise
it; a host runtime is a legitimate optional library — but even then the right
shape is a WIT package plus a host implementation, with the C header as one
generated binding, not a new primitive in `WasmBase/`.

> **The wire surface is largely redundant; the object model is not; neither
> belongs in the engine.**

## 3. What the scenario does not demand

### 3.1 An engine object

No header, no symbol and no `WasmBase/` change is implied by §2.1. The engine's
job is to keep the *value* transportable — a pointer-sized word, an `externref`, a
GC reference or a component resource — and that is shipped ([`../ABI.md`](../ABI.md)
“Host boundary”, “GC Object and Heap ABI”). The table, the descriptor registry and
the lifetime policy are the embedder's, exactly as
[`../LAYERING.md`](../LAYERING.md) §4 requires of anything that describes a
product's objects.

### 3.2 A new type system

A synthetic object needs identity, and the engine already has two identity
domains — plus the component one — and none of them is a candidate for reuse as a
global object key:

- **`TypeId` is Store-scoped**, dense, structural over closed Wasm types, and
  assigned by `TypeRegistry`; it is not a name, not process-global, and not stable
  across Stores or runs ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.1/§6.2).
- **A GC object's header stores that `TypeId`**; `ref.test`/`ref.cast` read it
  ([`../ABI.md`](../ABI.md) “GC Object and Heap ABI”, [`GC.md`](GC.md) §4.3).
- **Component types have their own domain**: Store `TypeId`s for closed
  definitions, `SchemeId` for alpha-equivalent open types, nominal per-instance
  resource identity ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §10,
  [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11).

So a synthetic object needs a **third domain**: a host registry keyed by a stable
name and a versioned descriptor, with its own ids. It may *map* a host type to a
Store `TypeId` when the object is a Wasm GC type, but it must not use `TypeId` as
a global key, and a handle must say which domain it belongs to. The descriptor
shape is studied in [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §13.

### 3.3 One shared object

“Silently wrapped in a GC language” has three distinct meanings, and only two of
them are a shared object:

| Case | What the GC language sees | What the wrapper does | Shared object? |
|---|---|---|---|
| **1. Same canonical Wasm GC type** | a real `(ref $T)` / `(ref null $T)` emitted by its compiler | nothing at runtime; the host supplies factories/accessors | **yes** — same `TypeId` in the same Store; `ref.cast` and field access work |
| **2. Language object wrapping a handle** | a Kotlin/MoonBit/Dart class whose private field is an `externref` or `u64` | every method forwards to the host runtime; identity is the wrapper's | **no** — the Wasm types differ; the object is behind the handle |
| **3. Component resource binding** | a generated `own`/`borrow` resource type | the toolchain generates the wrapper; the host is the `rep` | **at the component boundary** — nominal resource identity, not a core `(ref $T)` |

Case 1 requires the C side to be a host adapter (or a C toolchain extension that
emits GC types); the ordinary C guest is not in it. Case 2 is the usual “silent”
wrapper — an adapter, not a shared object model. Case 3 is the Component Model's
version and the most portable.

“Silent” can therefore mean **the generated binding hides the handle from the
application programmer**. It cannot mean **two language runtimes agree on what the
object is**. Kotlin's dispatch, MoonBit's runtime and Dart's object model remain
their own; only the handle and the exported functions are common.

### 3.4 A fixed backing

The object may live in host memory, in a Store `GcHeap`, or in a shared linear
memory. The choice is visible in the handle and must not be mixed; the rule is
**one owner per object graph**, because a host count and a GC count on the same
object is a double-free waiting for the first missed barrier. The two shipped
strategies are host-owned and guest-GC-owned (Appendix A.4), and the third is the
arena of Appendix A.13, which only the toolchains that can import a memory can
use.

Collection is not a synthetic-object operation. A guest-GC graph is collected at
the engine's safepoint (`Runtime::safepoint()`); a host-owned graph follows the
host's release policy. The runtime must not run a second collector over the
engine's heap, and it must not treat a guest GC pointer as a host object.

## 4. What we have implemented

**Snapshot (2026-11-18, working tree on `dcec0a7`):** the primitive and the
measurement below are shipped; the object model is not, by decision (§7).

### 4.1 The primitive

| Piece | What it gives a synthetic object | Anchor |
|---|---|---|
| `externref` + `HostBridge` | an opaque host value a guest can hold, retain and release | [`../WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp); [`../ABI.md`](../ABI.md) “Host boundary” |
| `GcHeap` + `ModuleInstance::allocateStructObject`/`allocateArrayObject` | a host-created Wasm GC object, owned by the instance's Store | [`../ABI.md`](../ABI.md) “GC Object and Heap ABI”; [`GC.md`](GC.md) |
| component resources | `own`/`borrow`, `resource.drop`, `resource.rep`, host handles | [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §2.3/§4.3 |
| native imports and the registry resolver | the function table a handle-plus-functions wire needs | [`../WasmNativeImport.hpp`](../../WasmBase/WasmNativeImport.hpp); [`../WasmRegistryImportResolver.hpp`](../../WasmBase/WasmRegistryImportResolver.hpp) |

### 4.2 Type identity is already owned

§3.2's third domain is the only piece a host registry has to *add*; the two Wasm
domains are implemented, tested and documented in
[`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md), and a GC object's header carries its
`TypeId` today. Nothing in this note proposes a change to either.

### 4.3 What deliberately does not ship

No handle table, no descriptor registry, no reflection, no properties and no
signals. The engine ships the transport and the resource contract; it ships no
product object model ([`../SCOPE.md`](../SCOPE.md) §3.2). The wire half is

## 5. Gaps

Every row below is a **claim that has not been demonstrated**, not engine debt:
the decision in §7 says nothing here is `WasmBase/` work. Severity is stated
against the scenario (“can a product ship this?”), not against the engine's
completeness meter ([`../SCOPE.md`](../SCOPE.md) §2).

| # | Gap | Evidence | Severity |
|---|---|---|---|
| SO1 | **No handle-lifetime test anywhere.** Create, use, release, reuse the slot, then use the stale handle; a wrong-type handle must be rejected before the host touches the object | nothing in the tree; the lab built the *wire*, not a table | High for a product; host-side |
| SO2 | **No RAII wrapper is exercised.** C++/Rust/Zig must release exactly once, including on an error path, and no exception or panic may cross the ABI | no generated wrapper exists (§4.3) | High for a product; tool-side |
| SO3 | **No GC-language wrapper test.** A Kotlin/MoonBit/Dart object holding the handle must reach a single decrement on drop, and a cycle through the wrapper must be collected or documented as a host cycle | case 2 of §3.3 is unbuilt | Medium; tool-side |
| SO4 | **Component resource parity is untested as a *parity* test.** `own` moves, `borrow` is scoped, `resource.drop` runs the destructor once, and a handle from another instance is rejected | the resource wires exist ([`COMPONENT_MODEL.md`](COMPONENT_MODEL.md)), but not against the same operation set | Medium; the redundancy test of §2.4 |
| SO5 | **No cross-Store identity test.** Two Stores with the same stable type name must yield different `TypeId`s and the same host descriptor, and a handle from one must be rejected by the other | descriptor shape is a study ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §13) | Medium; host-side |
| SO6 | **No ownership accounting test.** `HostBridge` retain/release counts must match the handle table; no double free on `instanceDestroying`; an object shared by two instances must have one host count | [`../WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp) has the hooks; no such test | High; host-side |
| SO7 | **No thread-affinity test.** Two agents calling one object without synchronisation must be refused or serialised by a documented policy | `THREADSAFETY.md` covers guest state, not host objects | Medium; host-side |
| SO8 | **No negative test.** A forged handle, a use-after-release, a raw pointer and a cross-runtime handle must all be clean errors | the rule is stated (§2.1); nothing measures it | High; host-side |
| SO9 | **No arena-object test** (Appendix A.13): instance A creates, instance B reads, A is unloaded and re-instantiated and the object survives; a forged offset, a freed slot and a cross-Store tag are clean errors | the arena needs the layout of [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) `SLM-1`…`SLM-4` first | Medium; host-side, gated on the sharing plan |
| SO10 | **No arena-ownership test**: the arena allocator and a host count must never both free one object, and a borrow must not outlive its owner | same gate as SO9 | Medium; host-side |
| SO11 | **No descriptor registry and no generator output exists** for the object case; the emitter work is §11/§14 of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) for functions and values, not for objects | closed by `SO-2` | High; tool-side |
| SO12 | **No transport for a host object that is not an `externref`** beyond the integer handle; `externref` is available in two of the tested toolchains (§8) | measured, §8 | Low; the `i64` wire covers all six |

## 6. Where we can and should play fast and loose

### 6.1 The deviations

- **SOD1 — the handle is an integer, not a reference type.** The alternative is a
  typed `(ref $T)`, which the C toolchain cannot declare and most of the measured
  languages cannot even spell untyped (§2.2, §8). An integer wire costs the type
  check — the host does it instead — and buys every language at once.
- **SOD2 — a third identity domain instead of reusing `TypeId`.** Reuse would look
  tidier and would be wrong: `TypeId` is Store-scoped and structural, so a host
  registry keyed by it would silently merge two types that happen to have the same
  shape and split one that moved between Stores (§3.2).
- **SOD3 — no `sizeof`, no `sizeof`-derived offsets, no C++ layout in a header.** A
  generated header carries accessors, not field offsets
  ([`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.7.1). It is the
  difference between an object and a struct whose layout the host cannot enforce.
- **SOD4 — one count per graph, never two.** A host count beside a GC count is the
  double free the engine's GC documentation exists to prevent ([`GC.md`](GC.md)
  `RC-1`); the second count is refused even when it would make a wrapper easier to
  write.
- **SOD5 — `externref` where it exists, `i64` everywhere else.** Measured, not
  assumed: two of six arms can spell a reference type (§8). A host that required
  `externref` for an object would exclude Rust, Zig, Go and TinyGo at these pins.
- **SOD6 — the engine ships no object model even though it could.** This is the
  same boundary as [`../LAYERING.md`](../LAYERING.md) §4 and
  [`../SCOPE.md`](../SCOPE.md) §3.2: the engine ships the opaque primitive, and a
  product object model belongs to the product.

### 6.2 The guardrails

A deviation is only safe while these hold: the handle is validated before every
access (generation, type, thread); the descriptor is versioned and the version is
checked; the wire stays `i64`-shaped so a new language needs no engine change; and
no deviation adds a second lifetime mechanism to an engine-owned graph.


## 7. Plan

**No milestone is opened.** The decision this note records is “study accepted,
engine implementation rejected” (Appendix A.9), so the plan below is what a
*product* would execute, in the order that avoids building the expensive half
first. Each row names the gaps it closes.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| `SO-1` | A WIT package with the resource types and the operation set of A.3, plus an optional host library implementing it outside `WasmBase/` | SO4, SO6, SO8 | a C guest compiled to a component creates, uses and releases a host object through generated bindings, and `resource.drop` runs the destructor exactly once |
| `SO-2` | Generated bindings and the descriptor registry — C first, then Rust, Zig, Go and TinyGo | SO2, SO11 | each language's header is generated from one descriptor source, and a version mismatch is a build- or load-time error, never a silent call |
| `SO-3` | GC-language façades (case 2 of §3.3), holding the handle in a language object | SO3, SO12 | one decrement on wrapper drop, and a documented answer for a cycle through the wrapper |
| `SO-4` | The arena backing (Appendix A.13), on top of `SLM-1`…`SLM-4` | SO9, SO10 | the object survives a writer's unload and re-instantiation, and a forged offset is a clean error |

The order matters for one reason: `SO-1` can be tested against an **existing**
contract (the Component Model's), while `SO-4` depends on a memory-layout plan that
is explicitly out of scope for the short term
([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §12.3, rungs 5–6).

## 8. Indicative test survey

The wire half was measured in the `SLM-6` lab ([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md)
§10; §15 is the evidence log): seven arms, one fixture per language, four `i64`
imports plus one export carrying an opaque handle, and a second variant carrying
`externref`. The lab is out-of-tree (`build/slm6`, gitignored), so this table is a
*measurement*, not a suite run.

| Arm | Toolchain (pinned) | Handle wire | `externref` |
|---|---|---|---|
| C | clang 23.1.6, `--target=wasm32 -nostdlib` | yes | yes, by default |
| C | clang 20.1.8 (the one Zig 0.15.2 ships) | yes | only with `-mreference-types`, otherwise `unknown type name '__externref_t'` |
| Rust | rustc 1.99.1, `wasm32-unknown-unknown` | yes | no such type |
| Zig | zig 0.15.2, `wasm32-freestanding` | yes | no such type |
| Go | go 1.27.4, `wasip1` | yes | no such type |
| TinyGo | tinygo 0.40.1, `wasip1` | yes | no such type |
| AssemblyScript | asc 0.28.19 | yes | only with `--enable reference-types` |

Reading: **the integer wire is universal; the reference wire is not.** Every arm
declares the same four `i64` imports, so case 2 of §3.3 has a working transport in
all six languages and no engine change is needed to serve them (Appendix A.12 has
the import section and the artifact sizes). The in-suite anchors are `[runtime]`
(180 assertions / 22 cases, 2026-11-18) for instance and image lifetime and `[gc]`
for the object header the guest-owned case relies on.

What this does **not** establish is exactly §5: no table, no wrapper, no
descriptor, no negative test.


## Appendix A: the GObject-shaped option, in full

This is the study as it stood in `EMBEDDING_SURFACE.md` §16, kept at its original
section numbers so an old reference still resolves. The parts that became
decisions moved into the slots above; this appendix carries what the slots cite.

| Old section | Where its content lives now |
|---|---|
| §16.1 Three layers | §2.2 |
| §16.2 What GObject contributes | §2.3 |
| §16.3 The shape of a host object runtime | A.3 |
| §16.4 Two backing strategies | A.4 |
| §16.5 What each language gets | A.5 |
| §16.6 “Silently wrapped in a GC language” | §3.3 |
| §16.7 Type identity | §3.2 and §4.2 |
| §16.8 Redundancy with the Component Model | §2.4 |
| §16.9 Feasibility verdict | A.9 and §7 |
| §16.10 What would make this a tested claim | §5 |
| §16.11 Ownership register | A.11 |
| §16.12 The wire claim, measured | §8 and A.12 |
| §16.13 Objects in the arena | A.13 |

### A.3 The shape of a host object runtime

Working name: **the host object runtime (HOR)**. No header or symbol is proposed;
the name is a placeholder for the design, and this note's “synthetic object” is
that design's unit.

Three layers: **wire** (a `u64`/`u32` handle, or an `externref` where the toolchain
can spell it — opaque, never dereferenced), **object** (a host table entry
`{type, generation, count, owner, payload}`, or a Wasm GC object with a `GcHeader`
`TypeId`), and **binding** (a C header, C++ RAII wrapper, Rust/Zig/Go/TinyGo
wrapper, and GC-language façades; generation is a convenience, the handle ABI is
usable by hand).

Three deployment choices, not interchangeable:

| Deployment | Where the runtime runs | What it can own | Best for |
|---|---|---|---|
| **Host library** | in the embedder process, reached through native imports | native host objects; `externref` payloads | existing C++ objects, OS/runtime integration |
| **Guest Wasm module** | compiled into the same Store, in linear memory or the GC heap | guest-memory objects only; no host pointers | portability, no host imports, one implementation for every guest |
| **Component package** | host implementation behind WIT resources | host objects, with `own`/`borrow` lifetimes | the standard cross-language contract, generated bindings |

A guest-side runtime is the most portable and the least powerful: every guest can
import it, but it cannot touch a host `std::string` or a native handle, and it must
bring its own allocator. A host-side runtime reaches native objects but needs a
per-language binding and a host ABI. A component package is the host-side runtime
with the Component Model as its contract — the recommendation (§7), which is why
“HOR” below means the host-side or component variant.

The minimum operation set is deliberately small:

| Operation | Contract |
|---|---|
| `create(type, ...)` | returns a handle, or null/typed error; type must be registered |
| `retain(handle)` / `release(handle)` | explicit ownership; release at zero runs the host deleter |
| `get_field(handle, field)` / `set_field(handle, field, value)` | validates type, field and generation before touching the object |
| `call_method(handle, method, ...)` | a host function table entry; no C++ vtable crosses |
| `type_of(handle)` / `is_a(handle, type)` | a runtime check against the host descriptor, not `ref.test` |
| `query_interface(handle, name)` | returns a handle for the interface, or null; no Wasm interface type |
| `weak(handle)` / `upgrade(weak)` | generation-checked; upgrade returns null after release |
| `drop(handle)` | releases one count; a component resource uses `resource.drop` instead |

The table is the host's, not the engine's. It must validate a handle before every
access (generation, type, thread). A forged integer is a normal C mistake and must
be a clean error, not a wild pointer (SO8).

### A.4 Two backing strategies, and the one-owner rule

A host runtime can own objects in two different ways, and the choice must be
visible in the handle:

| Strategy | Where the object lives | Who owns the lifetime | Best for |
|---|---|---|---|
| **Host-owned** | native host memory, referenced by a table entry | the host's reference count (or `HostBridge` for an `externref` payload) | existing C++ objects, GObject-like hosts, objects that must survive across instances |
| **Guest-GC-owned** | a Wasm GC `struct`/`array` in a Store's `GcHeap` | the engine's RC + cycle collector ([`GC.md`](GC.md) `RC-1`…`RC-5`) | objects created by GC languages, objects that need `ref.test`/`ref.cast` |

The rule is **one owner per object graph** (SOD4). A handle-table entry records
which strategy it uses, and an accessor checks that the handle's strategy matches
the operation.

The host *can* allocate a Wasm GC struct through the `ModuleInstance` allocation
entry points (`allocateStructObject`/`allocateArrayObject`, [`../ABI.md`](../ABI.md)
“GC Object and Heap ABI”), but that object belongs to the instance's Store and
needs the instance's module-local type index; it is not a general host heap. A
host-owned object is host memory; a guest-GC-owned object can also be created by a
guest factory and returned as an `externref`, or by a component resource. This is
a deliberate boundary, not a missing hook.

A **third** strategy becomes reachable once the memory is *shared*: the object
lives in the imported `LinearMemory`, the handle is an `i32` offset and the tag is
written by the host — the `GcHeader` without the collector (A.13), available only
to the toolchains that can import a memory
([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §10.3, Q1).


### A.5 What each language actually gets

| Target | What it sees | How it calls the runtime | Limit |
|---|---|---|---|
| **C** | an opaque `hor_object` typedef, or a `uint64_t` handle | generated/header functions; manual calls are first-class | no `sizeof`, no field offset, no typed `(ref $T)` |
| **C++** | the same handle in an RAII wrapper | wrapper calls `retain`/`release`; no exceptions or RTTI cross | the C++ class never crosses; no `shared_ptr` in the ABI |
| **Rust** | `#[repr(transparent)]` handle | `Drop` calls `release`; no `Deref` | no Rust `Drop` across the boundary; no `String`/`Vec` layout |
| **Zig** | `*anyopaque` or `u64` | explicit `deinit` | no guest finalizer |
| **Go / TinyGo** | `uint64` handle | `Close`/`release`; `//go:wasmimport` | Go's GC does not trace the host object; no Go pointer crosses |
| **GC language** | its own object/class, or a generated resource wrapper | the wrapper holds the handle and forwards | “silent” only at the source level; see §3.3 |

This is **B** in [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.4: a typed
handle table with generated ergonomics. It is a standardised version of the B
recipe, not a new C in the `LF2` scale. Calling it “C solved” is accurate only in
the sense that C can use the object; the C type system still has no typed GC
reference.

### A.9 Feasibility verdict

| Question | Verdict | Why |
|---|---|---|
| Can a host runtime give C/C++/Rust/Zig/Go/TinyGo a usable object API? | **Yes** | all can call C-linkage imports with integer/handle arguments; `HostBridge` and native imports already exist |
| Does it give them a typed `(ref $T)`? | **No** | that is a source-language/toolchain feature (`LG-1`); a handle is not a Wasm reference type |
| Can a GC language wrap it silently? | **Yes, as an adapter** | a wrapper object holds the handle; the wrapper's methods forward. It is not a shared object identity |
| Can C and Kotlin share the *same* object? | **Only in case 1 or 3 of §3.3** | same canonical Wasm GC type in one Store, or a component resource handle; not by both having a class named `Widget` |
| Can the Component Model replace the wire half? | **Yes** | resources + `own`/`borrow` + destructor + WIT bindings are the same contract, already implemented |
| Can the Component Model replace the object model? | **No** | no dynamic properties, signals, reflection, interfaces or class hierarchy |
| Is this an engine feature? | **No** | §7 and [`../LAYERING.md`](../LAYERING.md): the engine owns the opaque primitive, not a product object model |
| Is a first-party optional library feasible? | **Yes** | outside `WasmBase/`, versioned separately, built on `HostBridge`/`externref` or on component resources |

The cost is not in the handle table; it is in the **descriptor format, the
generator and the lifetime policy**. GObject's own history is the evidence: `GType`
made C objects introspectable, but cross-language use needed GObject Introspection
and per-language bindings. A host runtime would need the same second half, and WIT
already is that second half for components.

**Decision:** study accepted; **engine implementation rejected**; optional host
library / component package recommended; no milestone opened (§7). This is the same
boundary as [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.8, not a
reversal of it. If it is built incrementally, the order is §7's: contract and C
bindings first, dynamic properties and signals only if a product needs them — they
are the part with no standard contract and the part most likely to become a
language-specific runtime.


### A.11 Ownership register

| Item | Owner | Status | Where |
|---|---|---|---|
| `externref` payload, `HostBridge`, GC object header, component resources | **ENG** | implemented; no change proposed | [`../ABI.md`](../ABI.md), [`GC.md`](GC.md), [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) |
| Handle table, type registry, lifetime/thread policy | **EMB** | proposed, optional | A.3 |
| C/C++/Rust/Zig/Go/TinyGo headers and GC-language bindings | **TOOL** | proposed | `SO-2`/`SO-3`; [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §11/§14 |
| The handle **wire**, per toolchain (four `i64` imports + one export) | **TOOL** | **measured 2026-11-18** in six arms; `externref` only in C and AssemblyScript | §8, A.12 |
| C has no typed `(ref $T)` | **LANG** | accepted (`LG-1`) | [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §15.6 |
| Dynamic properties/signals/reflection | **EMB** | optional; not an engine feature | §2.4, [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.7.3 |
| Component resource contract | **ENG** + **TOOL** | engine side installed; WIT/bindings are tooling | [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §2.3/§4.3 |

No engine work is opened by this study. If a product later needs a host object
model, the first implementation is a WIT package and a host library; the engine
should be asked only for a missing primitive that cannot be expressed through
`externref`/`HostBridge`/resources, not for a reflection system.

### A.12 The measured wire, in detail

The import sections are the contract a host would have to implement:

```
;; h-c-handle.wasm (excerpt)
(import "env" "hor_create"    (func (;0;) (type 0)))   ;; (i32) -> i64
(import "env" "hor_retain"    (func (;1;) (type 1)))   ;; (i64) -> i64
(import "env" "hor_field"     (func (;2;) (type 2)))   ;; (i64, i32) -> i64
(import "env" "hor_release"   (func (;3;) (type 3)))   ;; (i64) -> ()
(export "use_handle" (func 4))
```

Artifact sizes: 355 B handle / 296 B `externref` (C, clang 23), 1,633,391 B
(Rust), 191 B (Zig), 1,910,747 B (Go), 102,349 B (TinyGo), 160 B handle / 105 B
`externref` (AssemblyScript). The smallest handle fixtures are 160 B, 191 B and
355 B; the megabyte-sized artifacts are fixtures that linked a language runtime,
not the handle ABI — the cost stays where A.9 puts it: descriptors, bindings and
lifetime policy.

The imports are ordinary `i64` functions — the surface `HostBridge` and native
imports already resolve ([`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §4.2,
§11) — so the handle wire needs **no engine change**, which is what A.9 said and
what the run did not alter.


### A.13 Objects in the arena

The third backing of A.4, and the one that exists because a memory can be
*shared*: the object is plain bytes in the imported `LinearMemory`, the handle is an
`i32` offset (plus a generation slot), the tag is written by the host, and the
lifetime is the arena's allocator. It is a `GcHeader` without the collector — and
the only backing that gives C, C++, Rust, Zig and AssemblyScript a zero-copy object
*and* object identity at once, because those five can import a memory
([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §10.3, Q1).

| Cost | Why the arena has it | Owner |
|---|---|---|
| **One allocator** | two `malloc`s over one heap hand out the same block (sharing note §9.1); the arena must be handed out by exactly one allocator | `SLM-3`; `SLM-12`'s front-end pass |
| **A layout** | the arena is a region, so each tenant needs a base and a heap that starts above it | `SLM-1`; the per-toolchain flags of §10.4 |
| **A growth policy** | a grow is invisible to the engine (every access reloads the base) but not to a partition plan | `SLMD3`; one nominated grower |
| **A tag that means something** | the engine's `TypeId` is usable inside one `Store` and meaningless after a reload in another process | §3.2; [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.1 |
| **Cheap rejection** | a forged offset must be a clean error, not a wild pointer, which needs the generation slot a host table would have given for free | §2.1; SO8 |

Two properties make it worth the cost. **It survives a reload**: the object is in
the arena, not in the instance's heap, so `Runtime::unload` and re-instantiation do
not destroy it — what a product that hot-reloads a mod wants. And **it needs no new
type system**: the wire is an `i32` every toolchain already has, while `externref`
exists in two of six (§8).

It is **not** [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.7.1's
rejected option, and the difference is the whole design: that section rules out a
*host pointer plus `sizeof(T)`* — a C++ class layout the engine cannot enforce and a
guest cannot dereference. An arena object is the opposite: guest-addressable bytes,
a layout fixed by a descriptor and generated accessors, no host pointer anywhere.
What it still cannot do is put a C++ *class* (vtable, RTTI, `std::string`) in the
arena; a C++ tenant wraps the offset, and that wrapper is a handle by another name
(A.5).

**Where the desugaring precedent fits.** The arena needs *allocation* to be
host-controlled, which is the front-end pass
[`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §13 scopes, with
[`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md) as its precedent: rewrite,
re-validate, prove nothing else changed, own a test suite. The precedent transfers
its shape and its limits — it rewrote opcodes with fixed semantics, while an
allocator pass matches per-toolchain symbol names
(`malloc`/`emmalloc_malloc`/`dlmalloc`/`__new`) and must leave an unknown module
alone. What no pass can supply is the object: the header, the field offsets and the
dispatch are IDL, not opcode rewriting. So:

> a desugaring pass can put the arena's **memory** under host control; it can never
> put the arena's **semantics** there.

That is `SO-4`'s gate, and it is why §7 puts the arena last.


## 9. References

- [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the embedder surface this
  note was extracted from: §4.2/§11 (the primitive and the generator plan), §12.4
  (the `LF2` A/B recipes), §12.7 (the non-options), §12.8 (the boundary), §14
  (generated headers), §15.6 (the language register), and the shortened §16 that
  now points here.
- [`../SCOPE.md`](../SCOPE.md) §3.1/§3.2 — the synthetic-module boundary, its
  diagnostics exception, and the synthetic-object boundary this note records.
- [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) — synthetic modules and host component
  definitions (§4.6), the resource contract (§2.3/§4.3), and the standard form
  §2.4 measures against.
- [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11 and [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)
  §6.1/§10/§13 — the identity domains, and the descriptor study a host registry
  needs.
- [`GC.md`](GC.md) — `RC-1`…`RC-5`, the object header, and the one-owner rule.
- [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) — §10 the measured
  per-toolchain layout study, §10.3 Q1 (who can import a memory), §12.3 (the linker
  ladder), §13 (the allocator-injection pass) and §15 (its evidence log).
- [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md) — the front-end rewrite precedent
  A.13 borrows.
- [`../ABI.md`](../ABI.md) — “Host boundary” (`externref`, `HostBridge`) and “GC
  Object and Heap ABI”.
- [`../LAYERING.md`](../LAYERING.md) §4 — why none of this belongs in `WasmBase/`.
- [`../PROPOSALS.md`](../PROPOSALS.md) — the shape of this note, and §2.3's
  references-last rule.
- The [GObject tutorial](https://docs.gtk.org/gobject/tutorial) — the precedent,
  including its cost.

measured, not assumed — §8.
