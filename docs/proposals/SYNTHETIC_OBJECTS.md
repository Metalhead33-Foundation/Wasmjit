# Synthetic Objects {#prop_synthetic_objects}

*Host-Registered Object Types, the GObject Precedent, and What the Component Model Already Covers*

Status: design note / implementation reference (**revision 2 — 2026-11-18**). This
is `EMBEDDING_SURFACE.md` §16 promoted to a note of its own: that section had grown
into a six-hundred-line study inside an implementation reference, and the
reference now keeps only the embedder-facing half. Revision 1 recorded a boundary —
*study accepted, engine implementation rejected*. **Revision 2 reverses the
implementation half and keeps the boundary where it belongs.** A host object model
is now a planned, opt-in feature (`SO-1`…`SO-5`); the identity it uses is the
Store's **`TypeId`**, not a third identity domain (`SOD2` is superseded); and the
engine gains a generic *type-registration and allocation* seam, while the object
model, its descriptors, its dispatch and its reflection stay outside `WasmBase/`
(`SOD6` rewritten, not deleted). **Contains no new code**: revision 2 is a decision
record plus the mechanism study in Appendix A.14. The `externref` + `HostBridge`
path is unchanged and remains the answer for truly opaque host objects; the new
path is a second, opt-in one for objects that are **expressible in Wasm**.

Audience: anyone designing a cross-language object layer, anyone asking whether
“C can use a Wasm GC object” is an engine gap ([`../SCOPE.md`](../SCOPE.md) §3.2),
anyone extending the generator plan of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
§11/§14, and the implementer of `SO-1`…`SO-5`.

Follows the same split as [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) and
[`TYPE_MODEL.md`](TYPE_MODEL.md). **Appendix A** carries the study itself, at the
section numbers it had as `EMBEDDING_SURFACE.md` §16.x (so an old reference still
resolves: 16.5 → A.5); A.14 is new in revision 2 and is the mechanism the decisions
now cite. §2–§8 state the decisions and cite the appendix rather than repeat it.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the scenario demand, now that host objects are a planned feature? |
| §3 | What does it *not* demand? |
| §4 | What is implemented today? |
| §5 | What is missing or untested (`SO1`–`SO16`)? |
| §6 | Where do we deliberately deviate (`SOD1`–`SOD7`)? |
| §7 | What is the plan (`SO-1`–`SO-5`)? |
| §8 | What does the test survey measure? |
| Appendix A | The study: a GObject-shaped host object runtime, in full (A.14 is the revision-2 mechanism) |
| §9 | References — the last section, as [§2.3](../PROPOSALS.md) requires |

## 2. What the scenario demands

### 2.1 The synthetic-object convention (revision 2)

A **synthetic module** is not a `Module`: it is a `(module, field)` resolver
convention whose entries are host-provided functions, memories, tables or globals,
and the guest sees ordinary imports ([`../SCOPE.md`](../SCOPE.md) §3.1,
[`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §4.6,
[`../languages/LF2_ABI_INTEROP.md`](../languages/LF2_ABI_INTEROP.md) §2).

A **synthetic object** is the object-side analogue, and revision 2 redefines it:
**an object of a host-registered Wasm type**. Its identity is the Store's
`TypeId`, its storage is the Store's `GcHeap`, and its name, fields, methods,
interfaces and lifetime policy come from a host-side descriptor that never
crosses the boundary. Read that as three sentences, because revision 1 said the
first one was false:

> The type is a canonical Wasm `struct`/`array` definition, interned into the
> `TypeRegistry` from a host-supplied definition, so a guest that declares the
> same shape receives the same `TypeId`. The descriptor is the *name* and
> *operation set* layered over that identity. The object model — dispatch,
> properties, signals, reflection, versioning — is the host's.

| The convention fixes | What it is | What it must **not** be |
|---|---|---|
| the type | a canonical Wasm struct/array, interned from a host definition → `TypeId` | a C++ class, a `GObjectClass`, a vtable |
| the name | a stable, versioned descriptor name mapped to that `TypeId` | an identity; a name does not make two Wasm types related |
| the handle | **two spellings of one identity**: `(ref $T)` where the toolchain can emit it, `i64` (a host-table slot) everywhere else | a pointer, a second object, a second count |
| the fields | Wasm value types only (`i32`/`i64`/`f32`/`f64`/`v128`/references) | `std::string`, a smart pointer, a non-trivially-copyable member |
| the dispatch | a host function table and generated accessors | a vtable, a `call_ref` on a hidden slot |
| the lifetime | the engine's one count per graph, plus explicit host roots (§3.4, A.14.4) | a host count beside the GC's |

Two consequences follow, and they are the point of the revision.

**A host-created object is a first-class Wasm object.** The host allocates it in
the Store's `GcHeap`, the header carries the `TypeId`, `ref.test`/`ref.cast` read
that header, `refClone`/`refDrop` maintain the count, and a guest can hold it in a
field, a table, a global or a local like any object its own compiler produced.
The GObject precedent is still the shape — a registered type with fields,
methods and interfaces — but the registration is now *structural* (a Wasm type
interned once) rather than nominal (`GType`), and the names live in the
descriptor (§2.5, §3.2).

**The wire spelling is not the identity.** `(ref $T)` and an `i64` handle name the
same object. A handle-table entry is a *root* — it holds one engine count through
`refClone` — not a second lifetime mechanism (`SOD1`, `SOD4`). That is how a
non-GC language uses an object a GC language created (A.14.4).

What it still is not is a C++ object. Nothing here puts a vtable, RTTI, a
destructor or a C++ layout in a descriptor; the constraint is §2.6 and the line
`SOD3`/`SOD7` draw.


### 2.2 Three layers, often conflated

The question “can C use a typed `(ref $T)`?” mixes three decisions:

| Layer | What it decides | Who can change it |
|---|---|---|
| **Wire representation** | one pointer-sized word per reference, or a `u32`/`u64` handle | the engine/backend ABI ([`../ABI.md`](../ABI.md)); already fixed |
| **Wasm type relation** | whether the value is `(ref $T)`, `externref`, `anyref`, a resource, and whether it `match`es | the Wasm spec, the guest toolchain, and — for a host-registered type — the Store's `TypeRegistry` |
| **Object model** | creation, fields, methods, inheritance, lifetime, threading; **identity moved up into the type relation in revision 2** | a host runtime, an embedder, or a language runtime |

Revision 2 moves *identity* out of the third layer and into the second, because a
host-registered Wasm type is interned like any other and dedups with a guest's
structurally identical declaration. What remains in the third layer is exactly
what the engine must not own: names, dispatch, properties, signals, versioning
and thread policy (A.14.1–A.14.3).

A synthetic object can own the third layer. It cannot change the second for a C
guest: a C compiler that does not emit GC reference types cannot declare a typed
`(ref $T)` import, no matter what the host provides. What it *can* do is make the
**value** usable — the guest carries an opaque handle and calls functions the
runtime implements. That is a change of API, not of type system.

This is why the honest target is not “C understands a Wasm GC object”. It is “C
can use a host object through a documented handle ABI, and a GC language can use
the same object through a typed reference or a generated wrapper” (§3.3 separates
the strengths).

**If a C compiler did gain a typed `(ref $T)` extension**, three things would
still be true: the C code would need GC rooting/barriers at every local and store;
field access would have to follow the Wasm GC layout, not a C `struct`; and the
type would be the canonical Wasm type, not a C tag. The host runtime would become
an implementation detail rather than the mechanism. That is a **LANG**/**TOOL**
project, not an engine feature.

“Everyone understands it” is likewise not a property a runtime can grant. It is
either a **shared canonical Wasm type** (structural, same `Store`) or a **shared
IDL/resource convention** (nominal, via WIT or a descriptor name). §3.3 separates
those meanings; §2.4 measures the Component Model overlap. With revision 2, the
first is the default path for a synthetic object and the second is the packaging
option — not the other way round.


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
| reference counting (`g_object_ref`/`unref`) | **yes** — one count per graph (§3.4) |
| interfaces (`GInterface`, `query_interface`) | **yes** — a host descriptor query, not a Wasm type |
| properties and `GValue` | **only as a dynamic feature**; a static field accessor is cheaper and is what most products need |
| signals | **expressible**, but as functions or function-typed parameters; not an engine feature |
| boxed types (`GBytes`/`GVariant`/`GArray`/`GMainLoop`) | **no new idea** — canonical records/lists/byte arrays, or the `ComponentExecutor` seam |
| the macro ergonomics | **no** — generated bindings are the modern replacement, and WIT already is one (§2.4) |

The transferable core is therefore the **dynamic, introspectable object model** —
registration, properties, signals, interface queries — not the container types or
the main loop, which the Component Model (or the executor seam) already covers.
**Revision 2 adopts that core**, minus the macro ergonomics and minus nominal
`GType` identity: the registration is a Wasm type intern (§2.1), the introspection
is a hand-written descriptor table (§2.5, A.14.5), and the ergonomics are
generated bindings (§2.4, A.14.5).

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

> **The wire surface is largely redundant; the object model is not; the identity
> and the allocation are the engine's, and the object model is not.**

That is revision 2's split. Revision 1 read the same evidence as “neither belongs
in the engine”; the difference is that a host-registered **Wasm type** is not a
new wire ABI — it is the type system the engine already has — so the redundant
half and the engine half turn out to be the same half (§3.1, A.14.3). A component
package behind WIT remains the recommended *packaging* for a product that wants a
nominal, specified boundary (§7 `SO-4`).

### 2.5 The five decisions of 2026-11-18

Revision 2 is a set of decisions, not a drift. They are listed once here, and
every later section cites the ID.

| # | Decision | What it replaces | Where the mechanism lives |
|---|---|---|---|
| **D1** | **Identity is the Store's `TypeId`.** A host type is interned structurally; a guest that declares the same shape gets the same id, and `ref.test`/`ref.cast` work across the boundary. | revision 1's third identity domain (`SOD2`) | A.14.2, A.14.3; [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.1/§6.2 |
| **D2** | **The host may create an object of that type** in the Store's `GcHeap`, receive a reference, pass it to a guest, and let the guest's count keep it alive. | “the engine never allocates a host object” | A.14.3; [`../ABI.md`](../ABI.md) “GC Object and Heap ABI” |
| **D3** | **One identity, two spellings.** `(ref $T)` where the toolchain can emit it; `i64` handle everywhere else. The handle table is a root, not a second object or count. | `SOD1`'s integer-only wire | A.14.4 |
| **D4** | **The descriptor is hand-written metadata and JSON, not RTTI.** Field/method tables are arrays of structs; the generator's envelope carries them and emits WIT or per-language bindings. | “no reflection at all” | A.14.5 |
| **D5** | **The feature is opt-in and does not replace `externref` + `HostBridge`.** A truly opaque host object (a `std::shared_ptr`, a JS realm object, a native window handle) still uses the opaque path. | `SOD6`'s “no object model, ever” | §3.1, §6 `SOD6` |

D1 and D2 are engine changes (class **B**: additive public surface, no removal);
D3–D5 are tooling/host work plus one ABI rule. None of them is a spec change: a
host-registered type is an ordinary Wasm type, and a host-created object is an
ordinary Wasm object ([`../ABI.md`](../ABI.md) “GC Object and Heap ABI”).

### 2.6 The constraint: the object must be expressible in Wasm

This is the line the model draws, and it is not negotiable:

> Every field is a Wasm value — `i32`, `i64`, `f32`, `f64`, `v128`, or a packed integer (`i8`, `i16`), or a reference
> — and every object is a `struct` or an `array`. If a C++ type cannot be written
> that way, it does not become a synthetic object.

| Expressible | Not expressible (and where it goes instead) |
|---|---|
| a `struct` of scalars and references, with `i32` offsets into a shared memory for bulk data | `std::string`, `std::vector`, `std::map` as *fields* — copy through a canonical string/list record, or store a handle |
| a reference to another synthetic object, including a cycle | a `std::shared_ptr` graph with a custom deleter — host-owned, behind a handle |
| a `struct` with a `v128` field | a type with a vtable, virtual bases, or RTTI — host-owned |
| an `array` of scalars or references | a non-trivially-copyable member, an RAII lock, a move-only type — host-owned |
| a component resource (nominal identity) | a C++ template instantiation — the descriptor names the instantiated type, the Wasm type is its fields |

“Non-trivially constructible/destructible” is the short version: if constructing
or destroying the object does work the guest cannot express, the object stays on
the host side and the guest gets a handle (`externref`, a component resource, or
an `i64` slot). The reason is not taste: a Wasm GC object has no destructor hook
that can run arbitrary host code, and the engine will not run a C++ destructor
from a collector (`SOD4`). A destructor that *must* run is a host responsibility,
either explicit (`destroy`) or from `instanceDestroying`/`resource.drop`.

The practical consequence for a product is a **narrow, declarative class
library**: plain data plus behaviours expressed as functions. That is the
GObject/Vala constraint, and it is the price of crossing the boundary at zero
copy. A product that wants to expose an arbitrary C++ object still has §12 of
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md).


## 3. What the scenario does not demand

### 3.1 An engine object model

The split is the whole decision: **identity and storage are the engine's; the
object model is not.** The engine interns the type (D1) and allocates the object
(D2); it does not know what a “property” is, it has no `setProperty(name)` call,
no signal table and no notion of inheritance beyond the Wasm type's declared
supertypes. Those are the host's, exactly as [`../LAYERING.md`](../LAYERING.md) §4
requires of anything that describes a product's objects, and they are where
`SOD6` still bites (§6.1).

Two things therefore stay true from revision 1: no header in `WasmBase/` names a
product's class, and the core does not grow a `GObject`-shaped subsystem (except possibly as an ergonomics wrapper). What
changed is that “the engine allocates the object” is no longer a boundary; it is
a primitive, and A.14.3 is its contract.

### 3.2 A new type system

Revision 2 removes the third identity domain. The reasoning that produced it was
correct about the facts and wrong about the conclusion:

- **`TypeId` is Store-scoped**, dense, structural over closed Wasm types, and
  assigned by `TypeRegistry`; it is not a name, not process-global, and not stable
  across Stores or runs ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.1/§6.2).
- **A GC object's header stores that `TypeId`**; `ref.test`/`ref.cast` read it
  ([`../ABI.md`](../ABI.md) “GC Object and Heap ABI”, [`GC.md`](GC.md) §4.3).
- **Component types have their own domain**: Store `TypeId`s for closed
  definitions, `SchemeId` for alpha-equivalent open types, nominal per-instance
  resource identity ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §10,
  [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11).

Those facts do not require a second registry; they require **not confusing a name
with an identity**. So:

| Need | Where it lives now | What it costs |
|---|---|---|
| a stable, human-readable class name | the host descriptor, keyed to a `TypeId` | a name is not a `GType`; two names may map to one id |
| structural dedup with a guest type | `TypeRegistry` interning (already implemented) | two descriptors with the same shape are *one* type — intended, and tested by `SO16` |
| a nominal relation (`is_a` beyond Wasm supertypes) | a host function over the descriptor | not `ref.test`; it cannot be `ref.cast` |
| an id stable across Stores | nothing; ids are per Store | one Store per process is the intended operation (§3.2.1) |

#### 3.2.1 “You can have several Stores; you should have one”

The technical rule is unchanged and must stay stated: `TypeId` is Store-scoped, a
bare `Callable` carries an id but not its Store, and a reference from one Store is
not valid in another ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.1,
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §13.2). Revision 2 adds the
operational rule that makes D1 safe:

> **The intended operation is one Store per process lifetime.** An application
> *can* create several Stores — the API permits it, and isolation/hot-reload may
> want it — but *should not*. A host object registry is therefore Store-owned, its
> `TypeId`s are the Store's, and a second Store gets a second registry with its own
> ids for the same names.

That is the honest price of reusing `TypeId` instead of inventing a global id: the
mapping is as global as the Store is, and the design says out loud what the
implementation already assumed.

### 3.3 One shared object

“Silently wrapped in a GC language” has three distinct meanings, and only two of
them are a shared object:

| Case | What the GC language sees | What the wrapper does | Shared object? |
|---|---|---|---|
| **1. Same canonical Wasm GC type** | a real `(ref $T)` / `(ref null $T)` emitted by its compiler | nothing at runtime; the host supplies factories/accessors | **yes** — same `TypeId` in the same Store; `ref.cast` and field access work |
| **2. Language object wrapping a handle** | a Kotlin/MoonBit/Dart class whose private field is an `externref` or `u64` | every method forwards to the host runtime; identity is the wrapper's | **no** — the Wasm types differ; the object is behind the handle |
| **3. Component resource binding** | a generated `own`/`borrow` resource type | the toolchain generates the wrapper; the host is the `rep` | **at the component boundary** — nominal resource identity, not a core `(ref $T)` |

Case 1 is now the **default** path for a synthetic object: the host registers the
type (D1) and creates instances (D2), so a GC guest holds a real `(ref $T)` with
the same `TypeId`. The C side still needs a host adapter or a toolchain extension
that emits GC types; the ordinary C guest uses case 2's handle (§3.3, A.14.4).
Case 2 is the usual “silent” wrapper — an adapter, not a shared object model. Case
3 is the Component Model's version and the most portable *packaging*.

“Silent” can therefore mean **the generated binding hides the handle from the
application programmer**. It cannot mean **two language runtimes agree on what the
object is**. Kotlin's dispatch, MoonBit's runtime and Dart's object model remain
their own; only the type, the handle and the exported functions are common.

### 3.4 A fixed backing

The object may live in a Store `GcHeap`, in host memory, or in a shared linear
memory. The choice is visible in the handle and must not be mixed; the rule is
**one owner per object graph**, because a host count and a GC count on the same
object is a double-free waiting for the first missed barrier. With D2 the **Store
`GcHeap` is the default backing for a synthetic object** — that is what makes
dedup, `ref.cast` and a host-created reference possible at all. The other two
remain available: host-owned for an opaque object that cannot be expressed in
Wasm (§2.6), and the arena of Appendix A.13 for a zero-copy object in shared
linear memory (A.4, A.13).

Collection is not a synthetic-object operation. A guest-GC graph is collected at
the engine's safepoint (`Runtime::safepoint()`); a host-owned graph follows the
host's release policy. The runtime must not run a second collector over the
engine's heap, and it must not treat a guest GC pointer as a host object.

### 3.5 A nominal object model

D1 makes the *identity* structural, which is not the same as making the *model*
structural. The model is nominal where it matters and it lives in the descriptor:

| Question | Answer under D1 | What it is not |
|---|---|---|
| “is this a `Widget`?” | a host `is_a` over the descriptor, or a `ref.test` against the registered `TypeId` | a `GType` check; there is no nominal registry in the engine |
| “does it implement `Drawable`?” | `query_interface(handle, name) -> handle` | a `(ref $I)`; Wasm has no interface types ([`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §13.6) |
| “what is its parent class?” | the descriptor's parent name, plus the Wasm type's declared supertypes | a second inheritance graph the engine checks |
| “can I add a field later?” | yes, as a new version → a new `TypeId` | a mutation of a registered type; `TypeId` identity includes the shape |

So the GObject *shape* survives — registration, interfaces, properties, signals —
while the GObject *mechanism* (`GType` as a generated per-class id and a global
nominal tree) does not. That is the deliberate trade: GType exists because C has
no other way to name a type at run time; a Wasm struct already has an identity,
and the descriptor supplies the name.

## 4. What we have implemented

**Snapshot (2026-11-18, working tree on `dcec0a7`):** the *primitives* below are
shipped and measured; the host-registration seam of D1/D2 is **not** — it is
`SO-1`, and its interface is scaffolded in §4.4. The snapshot is what lets the
note price the mechanism instead of promising it.

### 4.1 The primitives

| Piece | What it gives a synthetic object | Anchor |
|---|---|---|
| `TypeRegistry::internModule`/`internGroup` | canonical interning, `matches`/`matchesHeap`, `canonicalType` views — **module-originated only today** | [`../WasmTypeRegistry.hpp`](../../WasmBase/WasmTypeRegistry.hpp); [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6 |
| `GcHeap` + `ModuleInstance::allocateStructObject`/`allocateArrayObject` | a host-created Wasm GC object, owned by the instance's Store — **needs a module-local type index today** | [`../ABI.md`](../ABI.md) “GC Object and Heap ABI”; [`GC.md`](GC.md) |
| `GcHeader` + `refClone`/`refDrop`/`refMatchesHeapType` | the identity stamp, the count, and the cast the guest uses | [`../WasmGcHeader.hpp`](../../WasmBase/WasmGcHeader.hpp); [`../WasmModuleInstance.hpp`](../../WasmBase/WasmModuleInstance.hpp) |
| `externref` + `HostBridge` | an opaque host value a guest can hold, retain and release — unchanged, and still the D5 path | [`../WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp); [`../ABI.md`](../ABI.md) “Host boundary” |
| component resources | `own`/`borrow`, `resource.drop`, `resource.rep`, host handles | [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §2.3/§4.3 |
| native imports and the registry resolver | the function table a handle-plus-functions wire needs | [`../WasmNativeImport.hpp`](../../WasmBase/WasmNativeImport.hpp); [`../WasmRegistryImportResolver.hpp`](../../WasmBase/WasmRegistryImportResolver.hpp) |

### 4.2 Type identity is already owned

The identity half of D1 is implemented, tested and documented in
[`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md), and a GC object's header carries its
`TypeId` today. What is missing is not a new identity rule but a **public way to
originate a type that no module declared**: `internModule` consumes a parser's
`Subtype` vector, and `allocateStructObject` resolves a module-local index
(`SO13`). Every mechanism behind a host type exists; there is no entry point.

### 4.3 What deliberately does not ship, yet

| Not shipped | Why | Where it goes |
|---|---|---|
| a host-originated type intern | the registry is parser-fed today | `SO-1` (engine) |
| allocation of a host type without a module-local index | the instance API resolves a local index | `SO-1` (engine) |
| a descriptor registry and its versioning | host metadata, not engine metadata | `SO-2` (host + tool) |
| object entries in the binding schema | the envelope carries imports/exports only | `SO-2` (tool) |
| dynamic properties, signals, a nominal class tree | product policy, and no common core | stays out, permanently (`SOD6`) |
| a C++ class, RTTI, a destructor, a C++ layout | the §2.6 constraint | stays out, permanently |

The last two rows are the boundary that survives revision 2. The first four are
gaps with IDs, not boundaries (§5).

### 4.4 The scaffolding (2026-11-18)

`SO-1`'s interface is in the tree, so the shape can be reviewed and used before
the implementation exists:

| Header | What is there | What is not |
|---|---|---|
| [`../WasmBase/WasmHostType.hpp`](../../WasmBase/WasmHostType.hpp) | `HostTypeDefinition`/`HostFieldDefinition` (the Wasm storage vocabulary, no names), `HostTypeLayout`/`HostFieldLayout`, `hostTypeSupportAvailable()`, and `layoutHostType()` — **implemented** over `gcStructFieldOffset`/`gcStructSize`/`gcArrayDataOffset`, so it agrees with the GC heap by construction | `internHostType`, `allocateHostStruct`, `allocateHostArray`: declared, documented, and throwing `std::logic_error`. `retainHostObject`/`releaseHostObject` are non-throwing no-ops until an object can exist. |
| [`../WasmBase/WasmHostObject.hpp`](../../WasmBase/WasmHostObject.hpp) | `HostType` (definition + layout + optional `TypeId`) and `HostObject` (move-only, one engine count, scalar/`v128` field access by index) | `HostType::intern`, `HostObject::createStruct`, `HostObject::createArray` reach the throwing seam. A reference field is refused by the accessors, because reading its word as data would bypass `refClone`/`refDrop`. |

Both are embedder-tier headers (`EMBEDDING_SURFACE.md` §6.1.1), carry no name or
descriptor (`SO-2`), and are pinned by `Test/HostObjectTests.cpp` (`[hostobject]`):
the mixed-field layout equals the GC contract, an illegal definition is refused
before any state changes, and the stubs throw rather than pretending to succeed.
This is what §4.3 calls “not shipped, yet”: the interface is shipped, the
implementation is not.

## 5. Gaps

Every row below is either a **claim that has not been demonstrated** or a missing
primitive. Revision 2 splits them by owner: `SO13`–`SO16` are the new ones, and
`SO13` is the only engine row. Severity is stated against the scenario (“can a
product ship this?”), not against the engine's completeness meter
([`../SCOPE.md`](../SCOPE.md) §2).

| # | Gap | Evidence | Severity |
|---|---|---|---|
| SO1 | **No handle-lifetime test anywhere.** Create, use, release, reuse the slot, then use the stale handle; a wrong-type handle must be rejected before the host touches the object | nothing in the tree; the lab built the *wire*, not a table | High for a product; host-side |
| SO2 | **No RAII wrapper is exercised.** C++/Rust/Zig must release exactly once, including on an error path, and no exception or panic may cross the ABI | no generated wrapper exists (§4.3) | High for a product; tool-side |
| SO3 | **No GC-language wrapper test.** A Kotlin/MoonBit/Dart object holding the handle must reach a single decrement on drop, and a cycle through the wrapper must be collected or documented as a host cycle | case 2 of §3.3 is unbuilt | Medium; tool-side |
| SO4 | **Component resource parity is untested as a *parity* test.** `own` moves, `borrow` is scoped, `resource.drop` runs the destructor once, and a handle from another instance is rejected | the resource wires exist ([`COMPONENT_MODEL.md`](COMPONENT_MODEL.md)), but not against the same operation set | Medium; the redundancy test of §2.4 |
| SO5 | **No cross-Store identity test.** Two Stores with the same stable type name must yield different `TypeId`s and the same host descriptor, and a handle from one must be rejected by the other | the rule is §3.2.1 and A.14.2; nothing in the tree | Medium; host-side — the test that justifies §3.2.1 |
| SO6 | **No ownership accounting test.** `refClone`/`refDrop` counts must match the handle table's roots; no double free on `instanceDestroying`; an object shared by two instances must still have **one engine count**, not one per instance | [`../WasmHostBridge.hpp`](../../WasmBase/WasmHostBridge.hpp) has the host hooks and `refClone`/`refDrop` carry the engine count; no such test | High; host-side |
| SO7 | **No thread-affinity test.** Two agents calling one object without synchronisation must be refused or serialised by a documented policy | `THREADSAFETY.md` covers guest state, not host objects | Medium; host-side |
| SO8 | **No negative test.** A forged handle, a use-after-release, a raw pointer and a cross-runtime handle must all be clean errors | the rule is stated (§2.1); nothing measures it | High; host-side |
| SO9 | **No arena-object test** (Appendix A.13): instance A creates, instance B reads, A is unloaded and re-instantiated and the object survives; a forged offset, a freed slot and a cross-Store tag are clean errors | the arena needs the layout of [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) `SLM-1`…`SLM-4` first | Medium; host-side, gated on the sharing plan |
| SO10 | **No arena-ownership test**: the arena allocator and a host count must never both free one object, and a borrow must not outlive its owner | same gate as SO9 | Medium; host-side |
| SO11 | **No descriptor registry and no generator output exists** for the object case; the emitter work is §11/§14 of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) for functions and values, not for objects | closed by `SO-2` | High; tool-side |
| SO12 | **No transport for a host object that is not an `externref`** beyond the integer handle; `externref` is available in two of the tested toolchains (§8) | measured, §8 | Low; the `i64` wire covers all six |
| SO13 | **No public way to register a host-originated type *yet*.** The interface exists ([`../WasmBase/WasmHostType.hpp`](../../WasmBase/WasmHostType.hpp), §4.4) and `internHostType` throws; behind it, `TypeRegistry::internModule`/`internGroup` take a parser `Subtype` span and `allocateStructObject` resolves a `LocalTypeIdx` through the module | [`../WasmHostType.hpp`](../../WasmBase/WasmHostType.hpp), [`../WasmModuleInstance.cpp`](../../WasmBase/WasmModuleInstance.cpp) (`allocateStructObject`) | **High — engine; it is the gate on D1/D2** (`SO-1`) |
| SO14 | **No object descriptor in the binding schema.** The envelope (`wasmjit.binding/1`) carries `imports`/`exports`; there is no `types`/`objects` section, so the generator cannot emit a typed reference or a resource for an object | [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §11.8; `Tools/Bindings/BindingModel.hpp` | High; tool-side (`SO-2`) |
| SO15 | **No dual-surface test.** One object, two spellings: a `(ref $T)` export and an `i64` handle export of the same operation must reach the same object and the same count; dropping the last handle must not free an object the guest still holds | A.14.4; nothing in the tree | High; engine + tool (`SO-3`) |
| SO16 | **No structural-dedup test for host types.** Two descriptors with the same shape must intern to one `TypeId`; two versions of one name must intern to two; a guest declaring the shape must match without knowing the descriptor | A.14.2; [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.2 | **High — it is the evidence for D1** (`SO-1`) |

## 6. Where we can and should play fast and loose

### 6.1 The deviations

- **SOD1 — one identity, two spellings (revised).** Revision 1 fixed the wire as
  an integer because a typed `(ref $T)` is not declarable in C and not spellable
  in four of the six measured toolchains (§2.2, §8). Revision 2 keeps that as the
  *universal* spelling and adds the typed one where the toolchain has it: a
  GC-capable guest passes the same object as `(ref $T)`, a non-GC guest passes it
  as an `i64` slot, and both are the same `TypeId` with one count. The cost is a
  second export variant when a producer wants both (A.14.4); the benefit is that
  neither language is a second-class citizen.
- **SOD2 — reuse `TypeId` instead of a third identity domain (supersedes
  revision 1).** Revision 1 argued that reuse “would silently merge two types that
  happen to have the same shape and split one that moved between Stores”. Both are
  true, and revision 2 *accepts* them: a Wasm type **is** structural, so two
  identical shapes are one type — that is dedup, not an accident — and a second
  Store is a second id space. What makes it safe is the operational rule that one
  Store per process is the intended operation (§3.2.1), and the test that proves
  the split (`SO5`, `SO16`). The alternative was a nominal host id that no guest
  could use in `ref.cast`; it bought tidiness at the cost of identity.
- **SOD3 — no `sizeof`, no `sizeof`-derived offsets, no C++ layout in a header.** A
  generated header carries accessors, not field offsets
  ([`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.7.1). It is the
  difference between an object and a struct whose layout the host cannot enforce.
- **SOD4 — one count per graph, never two.** A host count beside a GC count is the
  double free the engine's GC documentation exists to prevent ([`GC.md`](GC.md)
  `RC-1`); the second count is refused even when it would make a wrapper easier to
  write. D2 does not change this: the host is a **root** (`refClone`), not an
  owner with a parallel counter, and the handle table drops its count through
  `refDrop`.
- **SOD5 — `(ref $T)` where it exists, `i64` everywhere else, `externref` only for
  opaque host values.** Measured, not assumed: two of six arms can spell a
  reference type (§8), and the integer wire covers all six. A host that required
  `externref` for an object would exclude Rust, Zig, Go and TinyGo at these pins.
- **SOD6 — the engine ships the identity, the allocation and the count, and no
  object model (rewritten).** Revision 1 read this as “no object model, ever”, and
  revision 2 overrides that half: a host-registered Wasm type is a primitive the
  engine can offer without knowing a product. What survives is the boundary
  itself — no properties, no signals, no nominal class tree, no reflection
  registry, no C++ class ([`../LAYERING.md`](../LAYERING.md) §4,
  [`../SCOPE.md`](../SCOPE.md) §3.2). The test is whether the engine can implement
  it without a product name in the header; interning a type and allocating an
  object pass, `setProperty("colour")` does not.
- **SOD7 — a descriptor may carry field offsets, and they are host metadata
  (new).** Revision 1 forbade offsets outright. Revision 2 separates two things:
  the canonical field offsets the engine already computes from the *Wasm type*
  (`structFieldOffsets`) are legitimate host metadata, used by the host to read or
  write a field in bulk and by the arena backing (A.13); a `sizeof(T)`-derived
  offset or a C++ member offset is still forbidden (`SOD3`). For a guest the
  offsets are **useless**, because a GC object is not a linear-memory address
  ([`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §13.1): guest access is
  through accessors, and no measured speed-up exists to
  trade against that. The performance answer is therefore: offsets help the
  *host* and the *arena*, never a GC guest (A.14.5).

### 6.2 The guardrails

A deviation is only safe while these hold: the handle is validated before every
access (generation, type, thread); the descriptor is versioned and the version is
checked; the wire stays `i64`-shaped so a new language needs no engine change; no
deviation adds a second lifetime mechanism to an engine-owned graph; **a host type
is interned through the Store and never mutated** (a field addition is a new
version → a new `TypeId`); and **the typed spelling never introduces a second
count** — `(ref $T)` and the `i64` slot are two views of one object, and the
engine's count is the only one.


## 7. Plan

Revision 2 **opens a milestone**, which revision 1 explicitly did not. The plan is
ordered so the engine seam is proved before any binding is generated, and so the
part that needs a memory layout (the arena) comes last. `SO-1` is class **B**
(additive public API, no removal); `SO-2`–`SO-4` are tooling/host work; `SO-5` is
host-side work gated on the sharing study and is **not** an engine milestone.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SO-1 — Host type registration and allocation** | Add a public path to intern a host-originated canonical type into the Store's `TypeRegistry` (the same interning that module types use), and a way to allocate/free an object of that `TypeId` in the Store's `GcHeap` with the header stamped and the count at its documented starting value. No descriptor, no names, no dispatch in `WasmBase/` (A.14.3). | SO13, SO16 | a test interns a struct from the host, allocates it, passes it to a guest export declared with the same shape, and `ref.test`/`ref.cast` succeed inside the guest; a structurally identical guest declaration gets the *same* `TypeId`, and two host versions of one name get two |
| **SO-2 — The descriptor and the schema** | A hand-written descriptor table (fields, methods, parent, interfaces, version) and an extension of the neutral envelope with a `types`/`objects` section, consumed by `Tools/Bindings/`. Emit WIT, and at least the C and Rust targets (A.14.5). | SO14, SO11 | one descriptor source produces a WIT package and a C/Rust header; a version mismatch is a load-time error; the generator never parses a C++ header for it |
| **SO-3 — The handle table and the dual surface** | A host-side handle table whose entry holds one `refClone` root, generated `i64` accessors, and the rule that a GC guest may pass the same object as `(ref $T)`. Explicit release, stale-handle rejection, no second count (A.14.4). | SO1, SO2, SO6, SO8, SO12, SO15 | one object is reached by a `(ref $T)` export and an `i64` export in the same run; dropping the handle does not free an object the guest still holds, and dropping the last guest reference does not free one the table still roots |
| **SO-4 — Component packaging, GC-language façade** | A WIT resource version of the same API, plus the generated wrapper for a GC language (Kotlin/Wasm or MoonBit) and a Dart/Kotlin-style façade holding the handle. | SO3, SO4 | `resource.drop` runs the host release once; a wrapper's drop reaches one decrement; a cycle through the wrapper is collected or documented as a host cycle |
| **SO-5 — The arena backing** | Objects as `i32` offsets in the shared memory of [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) A.13, gated on `SLM-1`…`SLM-4`. | SO9, SO10 | the object survives the writer's unload and re-instantiation, and a forged offset is a clean error |

**Scaffolding landed 2026-11-18.** `SO-1`'s interface is in the tree (§4.4:
`WasmHostType.hpp`/`WasmHostObject.hpp`, plus the `[hostobject]` test). The row
stays open and its `Done when` is unchanged: an interface that throws is not the
milestone.

Ordering, and why it is not negotiable: `SO-1` is the only engine row and
everything else is written against it — generating bindings for a type the engine
cannot intern would produce headers that cannot be tested. `SO-2` before `SO-3`
because a handle table without a descriptor is a table of anonymous slots.
`SO-4` is optional for a core-module product and mandatory for a component one.
`SO-5` is last for the reason revision 1 gave: it depends on a memory-layout plan
that is explicitly out of scope for the short term
([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §12.3, rungs 5–6).

The **arena-first** ordering revision 1 implied is therefore reversed. Revision 1
put a WIT package first because the Component Model was the recommended contract;
revision 2 puts the engine's type registration first because the identity is now
the engine's, and the Component Model becomes packaging (`SO-4`) instead of the
foundation.

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

That measurement answers the *wire* question and nothing else. Revision 2 adds
five claims the lab never tested, and they are `SO-1`–`SO-5`'s acceptance criteria:

| Test | What it would prove | Milestone |
|---|---|---|
| a host-interned struct, allocated by the host, cast by a guest | D1/D2: same `TypeId`, real `ref.test`/`ref.cast`, no adapter in the middle | `SO-1` |
| two descriptors of one shape, and one name at two versions | the dedup/split rule of A.14.2 is what actually happens | `SO-1` |
| one object through a `(ref $T)` export and an `i64` export | D3: two spellings, one identity, one count | `SO-3` |
| a stale, mistyped and cross-Store handle | the rejection is a clean error, not a wild pointer | `SO-3` |
| a generated C header and a generated WIT package from one descriptor | D4: the JSON is the source, the header is an artefact | `SO-2` |

What the lab still can **not** establish is the host-object model itself: it built
the transport, not a table, a descriptor, a wrapper or a negative test. That is
why §5 keeps the rows open even though §8 is green.


## Appendix A: the GObject-shaped option, in full

This is the study as it stood in `EMBEDDING_SURFACE.md` §16, kept at its original
section numbers so an old reference still resolves. The parts that became
decisions moved into the slots above; this appendix carries what the slots cite.
**A.14 has no §16 predecessor**: revision 2 added it, and it is the mechanism the
decisions of §2.5 now cite.

| Old section | Where its content lives now |
|---|---|
| §16.1 Three layers | §2.2 |
| §16.2 What GObject contributes | §2.3 |
| §16.3 The shape of a host object runtime | A.3 |
| §16.4 Two backing strategies | A.4 |
| §16.5 What each language gets | A.5 |
| §16.6 “Silently wrapped in a GC language” | §3.3 |
| §16.7 Type identity | §3.2 and §4.2; **superseded in part by A.14.2** |
| §16.8 Redundancy with the Component Model | §2.4 |
| §16.9 Feasibility verdict | A.9 and §7 |
| §16.10 What would make this a tested claim | §5 |
| §16.11 Ownership register | A.11 |
| §16.12 The wire claim, measured | §8 and A.12 |
| §16.13 Objects in the arena | A.13 |
| — (new in revision 2) | A.14 — host-registered types, the dual surface, and Poor Man's Reflection |

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
“GC Object and Heap ABI”), but today that object belongs to the instance's Store
and needs the instance's module-local type index. **Revision 2 makes that the
default path and removes the instance dependency** (`SO-1`): the object belongs to
the Store, its type is the host-registered one (`D1`), and its lifetime is the
engine's count. What is still *not* offered is a general host heap for arbitrary
C++ objects — the `GcHeap` allocates Wasm structs/arrays, not `std::string`s, and
that boundary is `SOD4`/`SOD6`, not a missing hook. A host-owned object is still
host memory; a guest-GC-owned object can also be created by a guest factory and
returned as an `externref`, or by a component resource.

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
| Is this an engine feature? | **Partly — and that is the revision-2 change.** The *identity* (interning a host type) and the *allocation* (a Store `GcHeap` object of that type) are engine primitives, because no host can do them without a private type system. The *model* — properties, signals, names, dispatch, reflection — is not (§3.1, `SOD6`) |
| Is a first-party optional library feasible? | **Yes** | outside `WasmBase/`, versioned separately, built on the SO-1 primitives, the handle table and (optionally) component resources |

The cost is not in the handle table; it is in the **descriptor format, the
generator and the lifetime policy**. GObject's own history is the evidence: `GType`
made C objects introspectable, but cross-language use needed GObject Introspection
and per-language bindings. A host runtime would need the same second half, and WIT
already is that second half for components.

**Decision (revision 2):** study accepted; **the identity and allocation half is
accepted as engine work** (`SO-1`), the object-model half stays out, and the
feature is opt-in (`D5`). The plan is §7's; the order is engine seam → descriptor
and schema → handle table and dual surface → component packaging → arena. Dynamic
properties and signals remain last: they are the part with no standard contract and
the part most likely to become a language-specific runtime.

That is a deliberate reversal of revision 1's verdict and it keeps
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.8's boundary intact: §12.8
rejects a C++ object model in the engine, and a host-registered Wasm type is not
one — it is the type system the engine already implements, with a new origin.


### A.11 Ownership register

| Item | Owner | Status | Where |
|---|---|---|---|
| `externref` payload, `HostBridge`, GC object header, component resources | **ENG** | implemented; no change proposed | [`../ABI.md`](../ABI.md), [`GC.md`](GC.md), [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) |
| Host-originated type intern + Store-`GcHeap` allocation of a host type | **ENG** | **planned** (`SO-1`, class B) | A.14.3; §5 `SO13`/`SO16` |
| Descriptor registry, handle table, lifetime/thread policy | **EMB** | proposed, optional | A.3, A.14.2, A.14.4 |
| C/C++/Rust/Zig/Go/TinyGo headers, GC-language bindings, descriptor JSON | **TOOL** | proposed | `SO-2`/`SO-3`; [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §11/§14 |
| The handle **wire**, per toolchain (four `i64` imports + one export) | **TOOL** | **measured 2026-11-18** in six arms; `externref` only in C and AssemblyScript | §8, A.12 |
| C has no typed `(ref $T)` | **LANG** | accepted (`LG-1`) | [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §15.6 |
| Dynamic properties/signals/reflection *in the engine* | **EMB** | stays out, permanently (`SOD6`) | §2.4, [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §12.7.3 |
| Component resource contract | **ENG** + **TOOL** | engine side installed; WIT/bindings are tooling | [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) §2.3/§4.3 |

Revision 2 opens **exactly one** engine row — host type registration and
allocation (`SO-1`) — and leaves every other row where revision 1 put it. The
engine is still not asked for a reflection system: the descriptor, the names and
the dispatch are host metadata, and `WasmBase/` gains a type origin and an
allocator entry point, not a class model.

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


### A.14 The revision-2 mechanism

This is the section revision 2 added: what “a host-registered Wasm type” means in
mechanism terms, how the two spellings stay one object, and what “Poor Man's
Reflection” is (and is not). It is a study, not an implementation: the engine
entry point it needs is `SO-1`, and the descriptor format it describes is `SO-2`.

#### A.14.1 What is registered

The registered unit is a **type definition**, not a C++ class:

- one Wasm `struct` or `array` (or a small `rec` group when the definition is
  recursive), whose fields are Wasm storage types — `i32`, `i64`, `f32`, `f64`,
  `v128`, or a reference to another type in the group;
- an optional declared supertype list — it participates in the canonical
  identity, so a host type cannot be “the same as” a guest type that declares a
  supertype the host omitted;
- field mutability and reference nullability, which also participate in identity;
- for an array, an element type and a mutability flag.

The host writes that definition once, in a declarative form. The shape below is
**host-side pseudocode**, not a proposed engine header:

```cpp
// Host-side description; the engine only sees the resulting canonical type.
HostType widget = hostType("demo.Widget", /*version=*/1)
    .field("x",      StorageType::I32)
    .field("y",      StorageType::I32)
    .field("colour", StorageType::I32)
    .field("parent", Reference::nullable("demo.Widget"))
    .build();
```

What is **not** part of the definition, and never will be: a vtable, a
destructor, RTTI, a `std::string` field, a smart pointer, a base-class layout, or
an offset derived from `sizeof(T)`. That is §2.6's constraint and `SOD3`'s
boundary. If a C++ type needs any of those, it is a host-owned object behind an
`externref`/resource handle (`D5`), not a synthetic object.

The reason to be strict is that the whole value of the exercise is that the guest
can hold the object *as a Wasm reference*. The moment the definition contains
something the Wasm type system cannot express, the object stops being a Wasm
object and the handle path is the honest answer.

#### A.14.2 Dedup, names and versions

Because the identity is structural, the interesting cases are all about what
“same” means. The rule is the one [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)
§3.2 already implements for module types; a host type is not special.

| Situation | Result | Why |
|---|---|---|
| host registers `{i32, i32}`, a guest declares `(struct (field i32) (field i32))` | **same `TypeId`**; `ref.test`/`ref.cast` succeed in the guest | structural identity over the canonical form — the dedup that makes the model worth having |
| the guest's struct has an extra declared supertype | **different `TypeId`**; the cast fails | the supertype list is part of identity (§6.2) |
| two descriptors of the same shape, names `demo.Point` and `geom.vec2` | **one `TypeId`, two names** | names are a lookup key, not an identity (D1, §3.2) |
| one name at version 1 (`{i32, i32}`) and version 2 (`{i32, i32, i32}`) | **two `TypeId`s**; the name maps to the current version | adding a field is a new type, not a mutation (the guardrail in §6.2) |
| an object of version 1 still alive while version 2 is registered | the old object keeps the old id and still validates as version 1 | `TypeId` is in the header; nothing is retroactively rewritten |
| the same name registered into a **second Store** | a **different `TypeId`** | `TypeId` is Store-scoped; the registry is Store-owned (§3.2.1) |

A descriptor registry is therefore a `(name, version) -> TypeId` map plus the
operation table, and it is Store-owned. Two consequences a product should
internalise:

1. **Do not use a name as an identity.** If a feature must not accept a
   structurally identical foreign object, that is a *nominal* check (`is_a` over
   the descriptor, or a `query_interface` capability), not a `ref.cast`. This is
   the same distinction [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §13.4 draws
   between GType and Wasm.
2. **Do not expect a rename or a field reorder to be “the same type”.** Both
   change the canonical form; both are a new version, and objects created under
   the old one keep it. That is the price of a structural identity, and it is also
   what makes a silently mismatched call impossible.

#### A.14.3 The engine seam (`SO-1`)

Today the two halves exist but are wired to a module:

| Half | Today | The missing piece |
|---|---|---|
| canonical interning | `TypeRegistry::internModule(span<const Subtype>, span<const TypeGroup>, prefix)` / `internGroup(...)`; hash-conses the canonical serialization under `internMutex_` | an entry point that accepts a **host-originated** definition and runs the *same* canonicalisation — not a second canonicalizer |
| allocation | `ModuleInstance::allocateStructObject(size, localTypeIdx, descriptor)`; resolves `module->typeId(LocalTypeIdx)`, charges the Store budget, stamps `GcHeader::kTypeIdOffset`, starts the count at 0 | an allocation that takes a **`TypeId`** (or a Store-level type handle), derives `size`/offsets from the canonical record, and does not require a module or a `LocalTypeIdx` |

The contract `SO-1` has to satisfy, stated as obligations rather than as a
signature:

1. **One canonicalizer.** A host type and a module type must land in the same
   intern table, or dedup is a lie. The entry point may not serialize its own way.
2. **Immutable once interned.** A `TypeId` never changes meaning; a definition
   change is a new intern (A.14.2). The registry already has this property for
   module types; a host type inherits it.
3. **Store ownership, not instance ownership.** The object belongs to the Store's
   `GcHeap` and is charged to the Store's budget; a per-instance arena is a
   different, later feature (`instanceDestroying` remains the backstop, not the
   owner).
4. **The GC header contract is unchanged.** The `TypeId` is stamped, the count
   starts at 0 (a fresh object is *unowned* until a reference-typed slot retains
   it, [`GC.md`](GC.md) `RC-1`), `refClone`/`refDrop` are the only count
   operations, and `ref.test`/`ref.cast` read the header exactly as they do for a
   guest-created object.
5. **Allocation failure is a trap/report, never an abort.** The budget refusal and
   `OutOfMemory` path stay as [`../SAFETY.md`](../SAFETY.md) §2.3 requires; a host
   allocation is not allowed to `std::abort()`.
6. **No product vocabulary.** The names are `TypeId`, `Subtype`, `struct`, `array`
   — the engine's existing vocabulary. No `Class`, no `Property`, no `Signal`
   ([`../LAYERING.md`](../LAYERING.md) §2/§4; `SOD6`).

Two things the seam must **not** accept, because both would smuggle a second type
system in through the back door: a host-supplied **byte layout** (offsets are
derived from the canonical record, never taken from the host) and a
host-supplied **numeric type id** (the registry assigns it). The host's handle on
the type is the name-to-`TypeId` map it keeps in its own descriptor registry
(A.14.2).

#### A.14.4 One object, two spellings (`SO-3`)

The handle is not a second object. It is a **slot that roots the object**:

```
create()            allocate (count 0) → refClone → count 1 owned by the table
                    → handle = { index, generation }
(guest, GC)         receives the raw reference word; refClone/refDrop are the guest's
(guest, non-GC)     passes the handle to host functions; the host validates
                    (generation, type) and uses the resolved reference
release(handle)     refDrop once, invalidate the generation, free the slot
```

Three properties fall out, and they are the whole of `SOD4`/`SOD1` in practice:

- **One count.** `refClone` on creation is the table's root; `refDrop` on release
  removes it. A guest holding the object through `(ref $T)` retains it through the
  engine's own barriers. There is no host-side counter.
- **Two spellings, one validation.** A `(ref $T)` is checked by `ref.test` inside
  the guest; an `i64` is checked by the host on resolve. Both end at the same
  `TypeId`, so neither is a weaker path — they differ only in *where* the check
  runs.
- **The handle is not portable.** It is a slot in *this* host's table, in *this*
  Store. Passing it to another process is meaningless; passing it to another Store
  is a cross-Store reference and is rejected (`SO5`).

**The non-GC question, and why “two versions of every function” is not the
default.** Revision 1's sketch was to generate each operation twice: once taking
the handle, once taking the typed reference. It works, and it is the wrong default
because the two sides are not symmetric:

| Option | Import side (host function) | Export side (guest function) | Verdict |
|---|---|---|---|
| **A — one handle surface + generated wrappers** | one registration | one export; a GC language's generated wrapper takes `(ref $T)` and forwards | **default**: one ABI, one implementation, one test |
| **B — dual surface (`typed` + `_h`)** | free: the host registers the same body twice, the typed one taking the raw reference | doubles the producer's exported code and its semantics unless the tool generates a thunk | **opt-in**, for a producer that must serve a non-GC consumer at the ABI level |
| **C — typed-only + `extern.convert_any` façade** | — | requires a GC-capable adapter module for the non-GC side | the `EMBEDDING_SURFACE.md` §13.3 façade; useful, but it is a second module |

The rule that makes B safe is the same one that makes A sufficient: **the second
spelling is a thunk, never a fork.** A `_h` export is generated to resolve the
handle and call the typed function (or vice versa), so there is one implementation
of the operation and two signatures over it. A producer that hand-writes two
implementations has two bugs waiting to diverge, and the note does not recommend
it.

So the non-GC answer is: **C gets a handle; it does not get a duplicate of your
module.** A generated C header calls the handle surface, a generated Kotlin/MoonBit
wrapper calls the typed surface, and both reach the same object. When a non-GC
consumer must call a GC producer *directly*, the tool emits the `_h` thunk — and
that is a tooling decision priced per module, not an engine feature.

#### A.14.5 Poor Man's Reflection (`SO-2`)

“Reflection” here means exactly one thing: **hand-written metadata about a type,
in a table, that a program can walk.** No `typeid`, no `__class__`, no compiler
intrinsic, no RTTI-dependent regeneration of a class from a string. It is the
GObject/Vala level of introspection, and it is deliberately the *floor*, not the
ceiling.

A descriptor is three tables of plain structs:

```
type    { name, version, parent?, interfaces[],
          fieldFirst, fieldCount, methodFirst, methodCount }
field   { name, storageKind, offset, flags }      // offset derived from the canonical type
method  { name, signatureId, trampoline }         // a native import / host function id
```

`offset` is the only numeric layout fact, and its **source** is the rule that makes
this safe: it comes from the canonical type the engine interned
(`structFieldOffsets` / `structSize`), never from `sizeof(T)` (`SOD7`). A host that
writes the table by hand is writing an *assertion* about the canonical type, and
the assertion is checkable against the `TypeId` at load time.

That table is not a Wasm ABI. It is the host's own metadata, and the guest never
sees it unless the API deliberately exposes introspection functions (`get_property`,
`type_of`, `query_interface`).

**The JSON is the same envelope the binding generator already speaks.** The
neutral schema (`wasmjit.binding/1`, `EMBEDDING_SURFACE.md` §11.8) carries
`imports` and `exports`; revision 2 extends it with one section, and the generator
that already emits WIT, C, Rust and every other target of
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §11.6 consumes it unchanged:

```json
{
  "schema": "wasmjit.binding/2",
  "direction": "all",
  "module": "demo",
  "objects": [
    { "name": "demo.Widget", "version": 1,
      "fields": [
        { "name": "x",   "type": "i32", "offset": 8 },
        { "name": "y",   "type": "i32", "offset": 12 }
      ],
      "methods": [
        { "name": "resize", "params": ["i32", "i32"], "results": [] }
      ] }
  ],
  "imports": [], "exports": []
}
```

Two things this is *not*: it is not a new IDL (the `type` spellings are the core
Wasm storage types the schema already uses), and it is not an engine parser. The
host writes the tables — by hand, or with a small builder — and the JSON is how the
same metadata reaches the generator. **Nothing moves into `WasmBase/`**:
`Tools/Bindings/BindingModel.hpp` stays a tool type, because the engine must not
gain a schema model any more than it may gain a reflection registry
([`../LAYERING.md`](../LAYERING.md) §4, `EMBEDDING_SURFACE.md` §15.3 `EN-7`).

**What offsets buy, and what they do not.** This decides whether the metadata is
worth writing:

| Consumer | Does an offset help? | Why |
|---|---|---|
| the **host**, reading/writing a field through the `GcHeap` object | **yes** | the object is host memory; the host can read a field without a call, and bulk copies need the offsets |
| a **GC guest** | **no** | a GC object is not a linear-memory address; `i32.load` cannot reach it. The guest uses typed field access or accessors (`EMBEDDING_SURFACE.md` §13.1) |
| the **arena** backing (A.13) | **yes** | there the object *is* bytes in the imported memory, and the offset is the guest's addressing mode |
| a generated **accessor call** | no measurable gain either way | a JIT-compiled accessor is a direct call, not a lookup; the offset saves one call only when the host itself is the caller |

So the honest answer to “would a table of offsets be faster than function calls?”
is: **for a GC guest there is nothing to compare — it cannot use the offset at
all; for the host, yes; for the arena, it is the whole mechanism.** A descriptor
is worth having because it drives *generation* and the *arena*, not because it
speeds up a guest field read.

**Where the metadata comes from.** Three acceptable sources, none of them
compile-time reflection:

1. a declarative builder the host calls at start-up (the A.14.1 sketch);
2. the same builder plus the JSON as the interchange, so the generator and the host
   read one thing;
3. for a host that already owns an IDL, a small converter from that IDL into the
   envelope.

A macro that expands to the table is a convenience for (1), not a fifth mechanism;
it still produces plain arrays of structs.

#### A.14.6 What revision 2 changes, and what it does not

| Revision 1 said | Revision 2 says | Why |
|---|---|---|
| a synthetic object is not a `TypeId`, not a `GcHeader`, not a `TypeRegistry` entry | it is all three, and the descriptor is what it is not | the object is a Wasm GC object; that is what makes dedup and `ref.cast` work |
| identity needs a third domain | identity is the Store's `TypeId`; names live in the descriptor | a nominal host id cannot participate in `ref.cast` and divides the object from its guest twin |
| the engine never allocates a host object | the engine allocates one of a host-registered type (D2) | allocation is a primitive, not an object model |
| the integer handle is the only wire | `(ref $T)` and `i64` are two spellings of one identity | a GC guest should not pay for a non-GC guest's limitation |
| no reflection at all | hand-written descriptor tables and the existing JSON envelope | metadata is required for generation and the arena; RTTI is still forbidden |
| study accepted, engine implementation rejected | `SO-1` opens; `SO-2`–`SO-5` follow | the identity and allocation half is generic engine work |

What revision 1 got right and revision 2 keeps: the three layers (§2.2), the
GObject shape and its cost (§2.3), the Component Model redundancy measurement
(§2.4), `SOD3`/`SOD4`/`SOD5`, the six-arm wire measurement (§8/A.12), the arena
(A.13), and the boundary that the engine does not ship properties, signals or a
C++ class (`SOD6`, rewritten but not removed).

## 9. References







- [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the embedder surface this
  note was extracted from: §4.2/§11 (the primitive and the generator plan), §11.8
  (the neutral envelope A.14.5 extends), §12.4 (the `LF2` A/B recipes), §12.7 (the
  non-options), §12.8 (the boundary), §13.1/§13.2 (a GC reference is not a linear
  address; Store-scoped identity and the one-Store rule), §14 (generated headers),
  §15.3/§15.4 (the `EN-*`/`EM-*` registers), §15.6 (the language register), and the
  shortened §16 that now points here.
- [`../SCOPE.md`](../SCOPE.md) §3.1/§3.2 — the synthetic-module boundary, its
  diagnostics exception, and the synthetic-object boundary this note records.
- [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) — synthetic modules and host component
  definitions (§4.6), the resource contract (§2.3/§4.3), and the standard form
  §2.4 measures against.
- [`TYPE_MODEL.md`](TYPE_MODEL.md) §7.11 and [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md)
  §6.1/§10/§13 — the identity domains, and the descriptor study a host registry
  needs. **A.14.2 revises §13.3's “third identity domain”**: the domain table stays
  correct about what each id answers, but a host object that *is* a Wasm object
  belongs to the core domain, not to a third one. §13.4 (nominal GType versus
  structural Wasm) and §13.6 (interfaces) are what §3.5 and A.14.2 cite.
- [`../SAFETY.md`](../SAFETY.md) §2.3 — why a host allocation that exceeds the
  budget is a trap/report, never an abort (A.14.3, obligation 5).
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
