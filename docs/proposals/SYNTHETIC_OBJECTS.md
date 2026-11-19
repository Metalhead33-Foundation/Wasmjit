# Synthetic Objects {#prop_synthetic_objects}

*Host-Registered Object Types, the GObject Precedent, and What the Component Model Already Covers*

Status: design note / implementation reference (**revision 15 — 2026-11-19**).
This is `EMBEDDING_SURFACE.md` §16 promoted to a note of its own. The question is
whether a host may register an object type and allocate instances of it, and the
answer is an opt-in feature: a host object is an ordinary Wasm `struct` or `array`,
its identity is the Store's **`TypeId`** (not a third identity domain), and the
engine owns the type-registration and allocation seam. The object model — its
descriptors, dispatch, names, properties, signals and reflection — stays outside
`WasmBase/` (`SOD6`), and `externref` + `HostBridge` remains the path for a truly
opaque host object.

Shipped: `SO-1` (the seam), `SO-2` (the descriptor and the `wasmjit.binding/2`
schema), `SO-3` (the handle table and its emitted surface), `SO-6` (reference
fields) and `SO-7` (properties). Partly shipped: `SO-4` — the packaging and the
Kotlin/Wasm and C++ façades exist, but `resource.drop` does not yet reach the host
release (`SO21`). Gated: `SO-5` (the arena), on the sharing study's
`SLM-1`–`SLM-4`. The latest revision (§4.14, `SO22`) records where `objects[]`
comes from: it is authored, never derived, and the note walks author →
`--emit schema` → emit.

Audience: anyone designing a cross-language object layer, anyone asking whether
“C can use a Wasm GC object” is an engine gap ([`../SCOPE.md`](../SCOPE.md) §3.2),
anyone extending the generator plan of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
§11/§14, and the implementer of `SO-4`/`SO-5`.

Follows the same split as [`COMPONENT_MODEL.md`](COMPONENT_MODEL.md) and
[`TYPE_MODEL.md`](TYPE_MODEL.md). **Appendix A** carries the study itself, at the
section numbers it had as `EMBEDDING_SURFACE.md` §16.x (so an old reference still
resolves: 16.5 → A.5); A.14 is the mechanism the decisions cite. §2–§8 state the
decisions and cite the appendix rather than repeat it.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the scenario demand, now that host objects are a planned feature? |
| §3 | What does it *not* demand? |
| §4 | What is implemented today? |
| §5 | What is missing or untested (`SO1`–`SO21`)? |
| §6 | Where do we deliberately deviate (`SOD1`–`SOD7`)? |
| §7 | What is the plan (`SO-1`–`SO-7`)? |
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
| …and a note on staging: a reference **field** is refused at interning until the Store-level release walk lands (`SO17`, `SO-6`) | — |
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

### 2.7 An object with no fields: is it still a synthetic object?

The question §2.6 raises one step further: a native class whose state cannot be
written as Wasm values at all — an `std::vector`, an `std::string`, a vtable —
exposed with **methods and properties and no fields**. Is that an opaque handle,
or a synthetic object?

**Yes, it is a synthetic object — with one precision about which half is
synthetic.** The degenerate descriptor is legal end to end, and the reason is
that §2.6's constraint is satisfied *vacuously*: every field is a Wasm value
because there are no fields.

| Part | A fieldless synthetic object | Why |
|---|---|---|
| the Wasm type | `(struct)` with zero fields | a legal Wasm type; interning does not require a field, and `layoutHostType` returns `headerBytes == totalBytes` |
| the identity | the Store's `TypeId`, as for any host type | §2.1's definition — *an object of a host-registered Wasm type* — is met by an empty struct |
| the storage | the object header in the `GcHeap`, **no payload** | the native state lives host-side, addressed by the handle: §3.4's *host-owned* backing, which it explicitly keeps “for an opaque object that cannot be expressed in Wasm” |
| the surface | methods, properties, constructors and `release`, unchanged | `fields: []` loads, and every emitter projects the rest |

```cpp
// The fieldless case: the empty struct is the whole Wasm type.
const WASM::HostType session = WASM::HostType::intern(store, WASM::hostStruct({}));
WASM::HostObject object = WASM::HostObject::createStruct(store, session);
// layout().fields is empty and headerBytes == totalBytes; every member of the
// native class is reached through an accessor, because none of it is a field.

// The one-field fix, when two such classes must stay distinguishable.
const WASM::HostType widget = WASM::HostType::intern(store, WASM::hostStruct({
    WASM::hostValueField(WASM::ValueTypeCode::I32),   // kind
}));
```

What it is **not** is *transparent*: no guest can read a byte of the native
object, so every member must be a call. That is not a defect of the descriptor —
it is exactly the property §4.7 claims for a property and the Component Model
claims for every resource member. The honest name is a **synthetic object with a
host-private payload**: synthetic in its identity, its lifetime and its call
surface; opaque in its representation.

Three things follow, and the third is the one that bites:

1. **It costs nothing to try.** An empty field list interns, an object of it
   allocates, and `fields: []` in `wasmjit.binding/2` emits a C++ class whose
   members are its methods and properties. `[bindings][objects]` already emits
   exactly that shape (the `demo.Trap` case).
2. **The payload's lifetime is the handle's.** `HostHandleTable::release` (or the
   RAII `HostObject`) is the one place the native object dies; the engine's count
   covers only the empty header. `instanceDestroying` is the last resort, and
   `resource.drop` becomes the guest-side spelling once `SO21` is wired.
3. **A fieldless type carries no discriminating identity.** Identity is
   structural (D1), so *every* fieldless host type interns to the **one** empty
   `(struct)`: `demo.Widget` and `demo.Session` share a `TypeId`, `ref.test`
   cannot tell them apart, and neither can
   `HostHandleTable::resolve(handle, type)` — its check compares the two
   `TypeId`s and they are equal. Nominal identity then rests entirely on the
   descriptor name and on which table the handle is resolved against, and a
   handle carries no table identity (`SO20`).

The cheap fix for (3) is a field one did not want: a single `i32 kind`, or a
`version` counter, makes the type structurally distinct and every check above
starts discriminating again. If not even that is affordable, the class
distinction has to live above the boundary — one `HostHandleTable` per class, or
the descriptor's `(name, version)` as the namespace — because the engine cannot
invent a nominal tag its identity model does not have (§3.5).

**Where the answer is still no.** An object with no fields *and* no methods or
properties is a token, not an object: register nothing and pass an `i64` or an
`externref`. And if the guest must reach *inside* the native payload — index the
`std::vector`, compare the `std::string` — no descriptor shape helps: the object
stays behind the opaque path (D5, `externref` + `HostBridge`) or its data is
copied through a canonical list/string record (§2.6, A.4). The first of those two
is now a **generated** shape rather than a hand-written one: `"self":
"externref"` on a descriptor projects the same member tables over an opaque host
reference, with no fields, no size and no `_release` (§4.13).


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
| a reference field whose count is not the Store's (`externref`, `exnref`, `contref`) | a host bridge or the continuation machinery owns that count, and the Store has neither | `SO18`; documented limit of the engine seam |
| a *dynamic* property or signal (a name looked up at runtime), a nominal class tree | product policy, and no common core; a **declared** property is a function pair and ships (`SO-7`) | stays out, permanently (`SOD6`); the declared form is §4.7 |
| a C++ class, RTTI, a destructor, a C++ layout | the §2.6 constraint | stays out, permanently |
| a *runtime* descriptor registry in the engine | the tool's tables are host metadata; a Store-owned registry would be a second object model | stays out (`SOD6`); the host owns the map (`SO-3`) |

The last three rows are the boundary that survives revision 2. The first row is a
narrower staging limit than revision 2's blanket rule: a **GC** reference field is
a graph edge the Store can walk (`SO-6`), while a bridge-owned one is not.

### 4.4 The seam, implemented (`SO-1`, 2026-11-18)

`SO-1` is in the tree and exercised end to end. The pieces and where they live:

| Piece | Where | What it does |
|---|---|---|
| **interning** | `internHostType` over `TypeRegistry::internGroup` | converts the definition to a `Subtype` and runs the **same** canonicalizer a module uses — one `rec` group of one member. A module declaring the shape gets the same `TypeId` (A.14.2). |
| **allocation** | `Store::allocateHostStruct` / `allocateHostArray` | validates the composite, derives `size`/offsets from the canonical record, allocates from a **Store-owned** `GcHeap::RegionSet`, stamps the `TypeId`, starts the count at 0. |
| **count** | `Store::retainHostObject` / `releaseHostObject` + `GcRefcount` | the engine's one count (`SOD4`). The saturating arithmetic moved to [`../WasmBase/WasmGcHeader.hpp`](../../WasmBase/WasmGcHeader.hpp) so the instance path and the host path cannot disagree. |
| **reclamation** | `Store::collectHostObjects` | a count that reaches zero is **queued** — a guest frame may still hold an uncounted borrow — and recycled at the next safepoint. `Runtime::safepoint()` drains it after the instances' own collections. |
| **ergonomics** | [`../WasmBase/WasmHostObject.hpp`](../../WasmBase/WasmHostObject.hpp) | `HostType` (definition + layout + `TypeId`) and `HostObject` (move-only, one count, field and array-element access by index). |

Evidence: [`Test/HostObjectTests.cpp`](../../Test/HostObjectTests.cpp)
(`[hostobject]`) over [`Test/wasm_wat/host_object_probe.wat`](../../Test/wasm_wat/host_object_probe.wat).
The host interns `{i32, i32}`, allocates it, writes 20 and 22; the module's
`sum_pair` returns 42 (layout and identity), `is_pair` returns 1 (`ref.test` reads
the header), and `save` stores it so the object survives the host's release.
`liveObjectCount()` pins both counts: one object while the guest holds it, none
after `drop_saved` and a safepoint. `SO13` and `SO16` are closed by this.

**The remaining limit.** `internHostType` accepts a reference field whose count is
this Store's — any/eq/i31/struct/array/none/defined, plus the uncounted `funcref` —
and refuses one whose count belongs to a host bridge or the continuation machinery
(`externref`, `exnref`, `contref`; `SO18`), because the Store has no bridge to
release it. The definition and `layoutHostType` describe both, because `SO-2`'s
descriptor and `SO-5`'s arena need the metadata; only *interning* is gated.

### 4.5 The descriptor and the schema (`SO-2`, 2026-11-18)

The descriptor is shipped as *tooling*, which is what A.14.5 asked for: the host
writes the table, the generator consumes it, and `WasmBase/` gains nothing.

| Piece | Where | What it does |
|---|---|---|
| **the model** | [`../../Tools/Bindings/BindingModel.hpp`](../../Tools/Bindings/BindingModel.hpp) | `ObjectField`, `ObjectMethod`, `ObjectDescriptor` and `Schema::objects`, parsed from and formatted to the `wasmjit.binding/2` envelope |
| **the validator** | `Schema::fromJson` | builds a `WASM::HostTypeDefinition` from each descriptor and calls **`WASM::layoutHostType`** — the `SO-1` function — so the offsets are the heap's, not a second implementation |
| **the emitters** | [`../../Tools/Bindings/EmitObjects.hpp`](../../Tools/Bindings/EmitObjects.hpp) | one versioned block per `(name, version)` in the C header, the Rust module and WIT |

**The load-time checks** (all `std::runtime_error`, all pinned by tests): an
unknown envelope version; an `objects` section under `/1`; a declared `offset`
that disagrees with the canonical one (the message names both numbers); a repeated
`(name, version)`; and a field type outside the Wasm storage vocabulary. `offset`
is an *assertion*, so a hand-written descriptor may omit it — the tool derives it
and writes it back.

**The version rule.** Every emitted symbol carries the version
(`wasmjit_object_demo_widget_v1`, `demo-widget-v1`), and the C/Rust descriptor
tables carry `name` and `version` beside the offsets. Two versions of one name are
two blocks and two tables, so a host can compare the version it registered against
the one it was generated for and refuse a mismatch at load — A.14.2's
`(name, version) -> TypeId` map, on the host's side where it belongs.

**What WIT cannot say is noted, not dropped.** WIT forbids returning a
`borrow<T>`, so a reference field gets no getter (a mutable one gets a setter); a
non-object reference, a `v128` member and a member with more than one result are
each replaced by a `///` note in the emitted file. `parent` and `interfaces` are
carried in the model, the JSON and the comments, but they are not Wasm type
relations and WIT has no inheritance, so nothing structural is emitted for them.

Evidence: `Test/BindingSchemaTests.cpp` (`[bindings][objects]`, four cases) takes
one descriptor source and checks the canonical offsets, every refusal above, and
the three artifacts — the C header is compiled by `gcc`, the Rust module by
`rustc`, and the WIT is parsed by `wasm-tools component wit` when those tools are
present. `SO11` and `SO14` are closed by this.

### 4.6 Reference-field graphs (`SO-6`, 2026-11-18)

`SO17`'s blanket refusal is gone: a host object may hold **GC references**, and
releasing it drops them. The walk is the only new mechanism; it is not a second
collector.

| Piece | Where | What it does |
|---|---|---|
| **the queue entry** | `Store::PendingHostObject` | carries the object *and* the `TypeId` captured before the tag is stamped, because the walk needs the canonical type |
| **the slots** | `Store::hostChildSlots` | the same offsets `ModuleInstance::childSlotsOf` uses (`gcStructFieldOffset`, `gcArrayDataOffset`, `gcStorageBytes`), skipping packed fields and non-reference kinds |
| **the walk** | `Store::collectHostObjects` | iterative (`work` grows as children reach zero), with a resurrection check and the reclaimed tag as the "already queued" mark — so a self-edge or a diamond cannot queue a block twice |

**The counts.** A host object starts at 0 (`SO-1`), the host's handle is one count,
and a guest that stores the object into a field takes another. Releasing the
host's root leaves the field's count, so the object survives; releasing it later
queues it, and the walk drops the child — which may queue the child in turn. That
is exactly the acceptance case: `liveObjectCount()` returns to its baseline only
after the whole graph is gone, and `ModuleInstance::gcRefCount` pins the
intermediate counts.

**Cycles.** A cycle is *not* freed by this walk — every link keeps a count — but it
is still collected at a safepoint, by the instance's existing **trial-deletion**
pass: a guest field store is a `refDrop`, which registers the candidate, and the
trial pass handles any object in the Store's heap. The Store adds a walk, never a
second collector; a cycle built with no instance release at all would be a
host-side leak until Store teardown, which is recorded rather than hidden (A.14.4).

**The remaining limit** is `SO18`: an `externref`/`exnref`/`contref` field is
refused at interning, because its count belongs to a host bridge or the
continuation machinery and the Store owns neither. A `funcref` field is accepted:
it carries no count.

Evidence: `Test/HostObjectTests.cpp` (`[hostobject][gc]`) — the graph case builds
`holder -> leaf` through the guest, releases the roots in the order that matters,
and pins `liveObjectCount()` at `+2` (the field's count) and back to the baseline;
the cycle case pins `gcRefCount` 2/2 → 1/1, an empty release queue
(`totalRecycles() == 0`) and the safepoint's collection.

### 4.7 Properties: a value behind a function (`SO-7`, 2026-11-19)

A **field** is a slot: `layoutHostType` gives it an offset, the header emits
`_FIELD_X`, and the guest loads a word. A **property** is a pair of calls, and
that is a different promise with a different cost.

The distinction is not cosmetic. A status member that happens to be an `i32` is
a field — but the moment "set it" also means "tell the mixer", "invalidate the
label", "push a UI update", the member is no longer a word: it is an operation.
The same is true of a value that is *computed* (an area derived from a geometry,
a lazily formatted caption). Wasm has one primitive for "this read is a call",
and it is a function; the descriptor needs a way to say so without pretending
the member has a storage location.

| Piece | Where | What it says |
|---|---|---|
| **the input** | `objects[].properties[]` in [`../../Tools/Bindings/BindingModel.hpp`](../../Tools/Bindings/BindingModel.hpp) | `{ "name", "type", "get", "set"? }` — a value type and one required accessor, a second optional one |
| **the check** | `validateObjects` | the accessor is declared in `methods` and its signature is `() -> type` (getter) or `(type) -> ()` (setter); no member name is shared with a field or a method; no accessor serves two properties |
| **the artifacts** | [`../../Tools/Bindings/EmitObjects.hpp`](../../Tools/Bindings/EmitObjects.hpp) | C/Rust: an export-name macro plus a function-pointer typedef and a `wasmjit_object_property_meta` row; WIT: a `func()` member |

**A property owns no bytes.** It never reaches `HostTypeDefinition`, so it cannot
shift an offset or change `size`: `demo.Widget` is 40 bytes with its three
properties and 40 bytes without them, which a test pins rather than trusts
(§4.5's rule — the layout is derived, never asserted).

**The accessor is a member of `methods`.** A property is sugar over a function
that the object exports anyway; if the descriptor names a function the interface
does not declare, the load fails, and if it declares one with a different
signature, the load fails with both signatures in the message. This is the same
philosophy as an asserted offset: the descriptor is checked against itself, and
a schema cannot promise a binding the emitted artifact cannot make.

**WIT needs no new concept, and that is the point.** The Canonical ABI already
models a resource member as a call — `[method]x` and `[method]x-set` — so at the
component boundary there is no such thing as storage. A field and a property
project to the *same* WIT shape:

```wit
resource demo-widget-v1 {
  x: func() -> s32;                 // a field: a slot on the host side
  /// Property `status`: computed by `get_status`, not a stored slot.
  status: func() -> s32;            // a property: a call on both sides
  set-status: func(value: s32);
}
```

The difference survives only in the comment, and it should: from JavaScript the
guest reads `widget.status` in either case, while the C++ side runs a function in
one case and loads a word in the other. The accessor method itself is not emitted
a second time — the property *is* its projection, so one member has one spelling.

**What this is not.** It is not `setProperty("colour")`: the name is resolved by
the generator, not by a runtime string table, and there is no `GValue` (SOD6,
permanently). It is not introspection either — the emitted tables are the host's
own metadata, and a product that wants reflection walks them, as A.14.5
already describes.

Evidence: `Test/BindingSchemaTests.cpp` (`[bindings][objects]`, six cases) takes
one descriptor with three properties — a mutable scalar, a read-only scalar and a
read-only handle — and checks the model, the no-slot rule, the round trip, ten
refusals, the three artifacts and their real compilation, plus the WIT parse.

### 4.8 Signals: considered, and left as a pattern

A signal is a property plus fan-out: one set, many listeners. Three shapes were
weighed, and only one of them is free.

| Shape | Needs | Verdict |
|---|---|---|
| **static subscription**: `subscribe(handler)` / `unsubscribe(handler)` as ordinary methods, the guest calls the host's dispatcher | nothing new — `methods[]` plus a `funcref` parameter, which the engine already carries | **available today**, no schema element; the pattern is two methods and a host-side list |
| **the host raises the signal** between calls | a retained callback the host may invoke when no call is in flight | **refused** (`SO19`): it re-enters the guest outside the one-agent-per-call contract |
| **dynamic signals by name**: `connect("notify::status", cb)` | a runtime name table and a marshaller — GObject's model | **out** permanently (`SOD6`) |

The first row is the honest answer for a Qt-shaped product: emit `subscribe` and
`unsubscribe` as methods, let the host's dispatcher fan out **inside** the call
that changed the value, and the behaviour is indistinguishable from a signal
without the engine learning what one is. The second row is a real limitation, not
a policy: a callback table with lifetime rules and a stated re-entrancy contract
is a proposal of its own, and it belongs beside
[`THREADS.md`](THREADS.md) rather than inside an object note.

### 4.9 The handle table and the dual surface (`SO-3`, 2026-11-19)

A.14.4 is now implemented, and it is deliberately small: the table is
*bookkeeping over one count*, not an object model.

| Piece | Where | What it does |
|---|---|---|
| **the handle** | `HostHandle` in [`../../WasmBase/WasmHostObject.hpp`](../../WasmBase/WasmHostObject.hpp) | `generation << 32` over the slot index, in one `i64`; 0 is the null handle |
| **the table** | `HostHandleTable` | `acquire(HostObject&&, HostType)` takes the count; `resolve(handle)` / `resolve(handle, type)`; `raw(handle)`; `release(handle)`; `liveCount()`/`slotCount()` |
| **the emission** | [`../../Tools/Bindings/EmitObjects.hpp`](../../Tools/Bindings/EmitObjects.hpp) | per object `…_handle`, `…_HANDLE_NONE` and one `_h_fn` typedef per method and property — the same signature with `int64_t self` |

**One count, and acquiring never clones.** A `HostObject` already owns the engine
count its allocation started with (`SO-1`); `acquire` *takes* that count into the
slot, so `liveObjectCount()` does not move and no `refClone` happens. Releasing
the handle is one `refDrop`, and a second release is a no-op rather than a second
drop — the table never had a counter of its own to get out of step (`SOD4`).

**Two spellings, one validation, two places to run it.** A `(ref $T)` is checked
by `ref.test` inside the guest; an `i64` is checked by the host on resolve. Both
end at the same `TypeId`, so neither is the weaker path — `resolve(handle, type)`
is the host's `ref.test`, and it refuses a handle of another type even when the
slot is live.

**A retired generation cannot be re-entered.** `release` bumps the slot's
generation, recycles the block at the next safepoint, and returns the index to a
free list; the next occupant gets a *new* generation, so the old handle resolves
to nothing while the slot is live again. A forged index, a forged generation and
the reserved generation 0 are all refused the same way.

Evidence: `Test/HostObjectTests.cpp` (`[hostobject][handle]`) runs the acceptance
case against a real module (`Test/wasm_wat/host_handle_probe.wat`): one object
reached as `(ref $pair)` (`sum_pair`, `is_pair`) and as the `i64` slot
(`handle_read`/`handle_write`, resolved by the host inside its own import), a
write through each spelling visible in the other, the handle released while the
guest still holds the object (it survives, then the guest's drop collects it), and
a *dead* handle reaching the guest's call path trapping with the host's reason
instead of resolving to something plausible. The second case is the negative
half: null, forged index, forged generation, generation 0, double release, a
foreign Store's object, a wrong type and slot reuse.

**What the emission is, and is not.** The `_h_fn` typedefs are *declarations* of
the thunk surface A.14.4 option B describes — resolve, then forward — so a
producer that must serve a non-GC consumer has one signature to implement per
operation and no second implementation to keep in step. They are not thunks: the
tool cannot write the resolve call, because the table is embedder state.

**One honest limit** (`SO20`): a handle carries no table identity, so a handle
handed to the wrong table either falls outside its slots or can alias a live slot
with a matching generation. The mitigation is structural — one table per Store,
the handle stays inside the host — and a table salt would make it checkable; that
is recorded rather than assumed away (§5).

### 4.10 Packaging and the GC-language façade (`SO-4`, 2026-11-19)

`SO-3` gave one object two spellings inside the host. `SO-4` is about the *other*
side of the boundary: a component-shaped package, and a wrapper for a language
that owns references rather than integers.

| Piece | Where | What it does |
|---|---|---|
| **the package** | `emitWitObjects` + the world in [`../../Tools/Bindings/EmitShared.cpp`](../../Tools/Bindings/EmitShared.cpp) | `interface types` carries one `resource` per `(name, version)`, and the `world` **imports** it: the host provides the objects, so `resource.drop` is the guest-side spelling of the host's release |
| **the façade** | `emitKotlinObjects` | one Kotlin/Wasm class per object: it holds the `i64` slot, exposes `raw`, and `close()` releases it **once** (`use { }` is the lifetime policy) |
| **the C++ façade** | `emitCppObjects` | one class per descriptor *in the C header itself*, after `extern "C"` closes: move-only, the destructor calls `…_release`, and `reset()`/`release()` spell the two ways to give the slot up |

**The direction is the decision.** A guest that consumes host objects imports the
interface; a component that *exports* objects would export it. Making that
explicit in the world is the whole of the packaging work — everything else about
a resource (its `rep`, its drop, its borrow rules) is already the Canonical ABI's.

**A declared constructor is a constructor.** `constructors[]` is its own table, not
a method with a special result: it has no `self`, its result is the object by
definition, and every emitter projects it as the host language's own construction
— `DemoWidgetV1(int32_t p0, int32_t p1)` in C++, `constructor(p0: s32, p1: s32)`
in WIT, `fun create(p0: Int, p1: Int)` in the Kotlin companion,
`<type>_<name>_h_fn` in Rust, and an entry point returning the slot in C. Three
things are load-time errors rather than a header that fails in a *consumer's*
build: a name that collides with a method (`<type>_<name>` is one namespace), two
constructors with the same parameter list (they would be one C++ constructor), and
a name spelled `release` (the lifetime entry point). A parameter list that *is* the
default or the adopting constructor's cannot be a second C++ constructor either, so
it becomes a static factory of the same name — `DemoNodeV1::spawn()` — which is the
same call spelled once. WIT has the opposite problem: every constructor is named
`[constructor]r`, with no signature in the name, so a resource carries exactly
**one**; the first expressible one is projected and the rest are noted with their
entry point (`wasm-tools component wit` rejects a second).

**The generator traffics structures, not JSON.** JSON is the boundary: the CLI
parses one document into `Schema`/`ObjectDescriptor`/`ObjectField`/`ValueType` and
writes one out, and from that point the object emitters see no `nlohmann::json` at
all — `EmitObjects.cpp` contains not one reference to it. The typed model also
carries the enums the emitters switch on (`ValueKind`, `HeapKind`), so “is this a
defined reference to `demo.Node`?” is a comparison, not a string match, and a
parameter's name arrives as a `std::vector<std::string>` rather than as a lookup
that can silently miss. Open-ended component payloads stay JSON by design (§4.5);
everything the generator *decides* on is typed.

**Parameters are named, and the name is declared.** A method or a constructor
carries `argNames`, the same index-aligned list a function entry uses, and every
emitter spells it — `int32_t width, int32_t height` in C and C++, `width: s32` in
WIT, `width: Int` in Kotlin, `width: i32` in Rust — with `pN` only for a parameter
that has no name. A list shorter than the signature is legal and does not shift the
others; a longer one, a duplicate name, and `self` are refused at load time.

**One header, two audiences.** The C header has always clamped its declarations
between `#ifdef __cplusplus / extern "C" {` and its closing `}`; what the handle
surface added is the *declarations themselves* — `<type>_release` plus one entry
point per method and property, the things the `_h_fn` typedefs describe. The C++
block then turns them into an object, so C gets a handle and C++ gets a class that
owns one, from the same file. Naming is a projection, not a second API: methods are
lowerCamel (`resize`, `area`), a property getter is the property's own name
(`status()`), its setter is `set<Property>` (`setStatus(7)`), and an accessor is
projected **only** as its property. Four names belong to the wrapper (`handle`,
`valid`, `reset`, `release`): a descriptor member that spells one is emitted as a
note naming its entry point instead, and `release` is refused at load time —
because it is the one name every generated artifact calls to end an object's life.

Evidence is compiled, not asserted: the emitter test writes the header, compiles a
C++ translation unit with `g++ -Wall -Wextra`, **runs** it, and counts releases —
a moved-from wrapper releases nothing, two `reset()`s release once, move-assignment
over a live slot releases the old one, and `release()` hands the slot back without
releasing. `static_assert`s pin copy-deleted and move-allowed for every descriptor,
including the ones with no members at all.

**The façade owns nothing the engine counts.** The wrapper is a `Long` plus a
`Boolean`: the table owns the count, so `close()` is exactly one
`HostHandleTable::release`, and the table's release is idempotent — the second
`close()` is guarded in the wrapper and would be a no-op in the table anyway.
That is what makes "a wrapper's drop reaches one decrement" checkable without a
Kotlin toolchain: the *link* the wrapper adds is tested in C++ (`[hostobject][handle]`
releases twice), and the wrapper's own text is asserted by the emitter test.

**What is not wired yet, and it has an ID.** The engine's component resource
destructor is proven to run exactly once in the defining runtime
(`component_resource_destructor` in the component suite). What does not exist is
the *plumbing* that makes `resource.drop` reach a `HostHandleTable`: it needs a
component-level import (`release: func(u32)`) lowered into the component's core
module, and a fixture that declares it. `SO21` records that precisely, and it is
the one item between `SO-4` as it stands and a product that hands objects to a
component through a resource.

Evidence: the emitter test (`[bindings][objects]`) asserts the world's import and
the façade's class, release call, idempotence guard and `HANDLE_NONE`; the WIT
still parses with `wasm-tools component wit`, which is what makes `import types;`
an assertion rather than a guess. The component half is the existing, green
`component resource destructor runs in the defining runtime` case.

### 4.11 The arena backing: not started, on purpose (`SO-5`)

`SO-5` is the last row, and it is **gated rather than deferred**: an arena object
is an `i32` offset into a shared linear memory (A.13), so the milestone cannot
start before the memory contract it indexes exists.

| Blocker | Where it is defined | Status |
|---|---|---|
| the build-side recipe (which side owns the memory, in what order) | [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) `SLM-1` | not started |
| the ownership and lifetime rule (creator outlives importers) | `SLM-2` | not started |
| the allocator contract (one allocator, disjoint partitions, or the toolchain's own) | `SLM-3` | not started |
| the four-case test set that makes the above executable | `SLM-4` | not started |

Writing an arena allocator for host objects *before* `SLM-3` would invent a second
allocator contract beside the one the sharing study exists to pin down — the exact
duplication this note rejects everywhere else (`SO-2`'s offsets, `SO-7`'s
accessor signatures, `SO-3`'s count). The milestone is therefore **not started**,
and the honest deliverable is this dependency list: when `SLM-1`–`SLM-4` land, an
arena object becomes "a `HostHandleTable` whose entry is a `{offset, size}` pair
in a memory the plan says is shared", and the acceptance case is already written
(`SO9`/`SO10`: the object survives the writer's unload, and a forged offset is a
clean error).

### 4.12 A tutorial: a synthetic object from C++, end to end

Everything in §4.1–§4.11 is already implemented and already exercised; this
section is the same walkthrough a reader needs to build one. The running example
is the smallest object that has all three member kinds — two fields, one method,
one property — and the walkthrough is the shape `[hostobject][handle]` runs.

**Step 0 — the native type.** Whatever the product already has:

```cpp
struct Widget {
    std::int32_t width = 0;
    std::int32_t height = 0;
};
```

**Step 1 — describe it in Wasm terms and intern it.** The description carries no
name, no version and no methods: those are descriptor metadata (§4.5), and the
engine only canonicalises the shape.

```cpp
auto runtime = WASM::Runtime::create("libjit", config);   // `config` carries the bridge: step 4
WASM::Store& store = runtime->store();

const WASM::HostTypeDefinition definition = WASM::hostStruct({
    WASM::hostValueField(WASM::ValueTypeCode::I32, /*isMutable=*/true),
    WASM::hostValueField(WASM::ValueTypeCode::I32, /*isMutable=*/true),
});
const WASM::HostType widget = WASM::HostType::intern(store, definition);
```

The `TypeId` is structural: a module that declares the same `(struct (mut i32)
(mut i32))` receives **the same identity**, so `ref.test` and `ref.cast` work
across the boundary with no adapter (`SO16`, §4.4). A field change is a new
definition and a new `TypeId` — never a mutation of this one.

`"libjit"` names a backend the way every backend is named: a code generator is its
own shared library, never linked into `WasmBase`; the application links it, its
registration hook runs, and `Runtime::create` selects it by name. A backend the
application does not link is reached through the same call — `create` `dlopen`s it
from `WASMJIT_BACKEND_PATH` — so this line does not change when the backend does
([`../BACKEND_PLUGIN_ABI.md`](../BACKEND_PLUGIN_ABI.md) §4.5).

**Step 2 — create an instance and fill it.** `HostObject` is move-only and owns
exactly one engine count (§4.4). The field index is the declaration index; the
byte offset comes from `layoutHostType`, which is the same function the generated
descriptor is validated against (§4.5).

```cpp
WASM::HostObject object = WASM::HostObject::createStruct(store, widget);
object.writeField<std::int32_t>(0, 320);
object.writeField<std::int32_t>(1, 200);
```

**Step 3 — root it in a handle table.** This is the half a non-GC guest needs
(`SO-3`, §4.9). `acquire` **moves** the count the object already owns: the handle
is a root, never a second object, so `liveObjectCount()` does not move and a
release through either spelling is the same decrement.

```cpp
WASM::HostHandleTable handles(store);
const WASM::HostHandle slot = handles.acquire(std::move(object), widget);
const std::int64_t wire = static_cast<std::int64_t>(slot.value);  // the i64 the guest carries
void* const reference = handles.raw(slot);                       // the same object, as (ref $T)
```

**Step 4 — serve the members from host imports.** A method is an import whose
parameter is the handle: `(func (import "demo" "widget_area") (param i64)
(result i32))` is `Widget::area()` at the boundary. The import is a plain function
taking the caller's `VMContext*` first, so per-instance state arrives through
`context->hostData` — never through a capture.

```cpp
struct HostState {
    WASM::HostHandleTable* handles = nullptr;
    const WASM::HostType* widget = nullptr;
};

std::int32_t widgetArea(WASM::VMContext* context, std::int64_t wire)
{
    auto* state = static_cast<HostState*>(context->hostData);
    const WASM::HostHandle handle{static_cast<std::uint64_t>(wire)};
    WASM::HostObject* object = state->handles->resolve(handle, *state->widget);
    if (object == nullptr)
        WASM::trap(WASM::TrapKind::Internal, "widgetArea: stale or foreign handle");
    return object->readField<std::int32_t>(0) * object->readField<std::int32_t>(1);
}
```

`resolve(handle, type)` is both checks in one call: a released slot's generation
has moved on, and a live slot of another type fails the identity comparison.
Register it, and let `nativeImport` be the trampoline
([`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §2.2):

```cpp
WASM::HostState state{};
state.handles = &handles;
state.widget = &widget;

WASM::HostBridge bridge;
bridge.userData = &state;
bridge.instanceCreated = [](void* userData, WASM::VMContext* context) {
    context->hostData = userData;      // published before `start` runs
};

WASM::RegistryImportResolver imports;
imports.registerFunction("demo", "widget_area", WASM::nativeImport<&widgetArea>());
```

The bridge must be installed **at `create`** (`RuntimeConfig::hostBridge`), not
after `instantiate`: global initialisers, segments and the start function run
during instantiation, so an import called from `start` sees `hostData` only if
`instanceCreated` published it.

**Step 5 — hand it to the guest.** Load, instantiate with the resolver, and pass
the `i64` exactly as the schema declares it:

```cpp
auto module = runtime->loadFile(path);
WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

const std::int32_t area = runtime->callExport<std::int32_t>(instance, "compute_area", wire);
```

A GC-capable guest may take the same object as a typed reference instead — the
second spelling of one identity (§4.9):

```cpp
const std::int32_t isWidget = runtime->callExport<std::int32_t>(instance, "is_widget", reference);
```

**Step 6 — release.** One call, and the slot's generation retires with it:

```cpp
handles.release(slot);   // every later resolve(slot) is nullptr
```

A `HostObject` that was never acquired still releases its own count in its
destructor, which is why the type is move-only. `runtime->safepoint()` at a frame
end collects the guest's objects as usual; a handle-rooted object survives it, and
a guest that drops its reference does not free a handle-rooted one (`SO15`).

**Step 7 — generate the other half.** The engine half above is hand-written; the
emitted half starts from a descriptor, and the two agree because the descriptor's
offsets are validated against the same `layoutHostType` (§4.5). Where that
document comes from is §4.14's question: it is authored, and the tool judges it.

```json
{ "schema": "wasmjit.binding/2", "direction": "all", "module": "demo",
  "objects": [ { "name": "demo.Widget", "version": 1,
      "fields": [ { "name": "width",  "type": "i32", "mutable": true },
                  { "name": "height", "type": "i32", "mutable": true } ],
      "properties": [ { "name": "status", "type": "i32",
                        "get": "get_status", "set": "set_status" } ],
      "methods": [ { "name": "area", "params": [], "results": ["i32"] },
                   { "name": "resize", "argNames": ["width", "height"],
                     "params": ["i32", "i32"], "results": [] },
                   { "name": "get_status", "params": [], "results": ["i32"] },
                   { "name": "set_status", "params": ["i32"], "results": [] } ],
      "constructors": [ { "name": "create", "argNames": ["width", "height"],
                          "params": ["i32", "i32"] } ] } ],
  "imports": [], "exports": [] }
```

```bash
./build/Desktop-Release/Tools/Bindings/wasmjit-bindings \
    --schema widget.binding.json --direction all --emit c-header > widget_object.h
```

The C header then carries `wasmjit_object_demo_widget_v1` (the opaque handle), the
field-offset macros, one `typedef` per method, the walkable metadata tables, the
`_h_fn` thunk declarations, and — inside `#ifdef __cplusplus` — one move-only
`DemoWidgetV1` class whose members are `area()`, `resize(width, height)` and
`status()`/`setStatus()`, and whose destructor calls `…_release`. The WIT target
gives the same object as a `resource` in an interface the world imports, which is
what a component-shaped package is (§4.10).

**Where it goes wrong.** Every row is a message the code above already produces:

| Symptom | Cause | Fix |
|---|---|---|
| `std::logic_error` (“a reference field is not writable as data”) | `writeField` on a reference slot | pass the reference through a method, or root it with the type's own helpers — reading that word as data would bypass the engine's count (§4.6) |
| interning refuses the definition by name | an `externref`, `exnref` or `contref` field | `SO18`: the Store does not own that count, so the object cannot be recycled; use a GC reference, or a bridge-owned value behind a method |
| `acquire` throws `logic_error` | the object's `TypeId` is not the given type's | intern once, keep the `HostType`, and give *that* to `acquire` |
| `acquire` throws `invalid_argument` | the object belongs to another `Store`, or the type is not interned | one runtime, one store (`ES-2`); intern before creating |
| `resolve(handle, type)` is null for a live handle | the handle was released, or a slot from another table aliased | that is the contract, not a bug: check for null; a handle carries no table identity (`SO20`) |
| two classes resolve as each other | both registered fieldless — one shared empty `(struct)` | §2.7: add the `i32 kind` field, or use one table per class |
| the import sees `hostData == nullptr` | the bridge was installed after `instantiate`, or `instanceCreated` is missing | set `RuntimeConfig::hostBridge` at `create` |
| a C++ exception escapes the import and the host dies | the import threw instead of trapping | `WASM::trap(...)` inside the body; `nativeImport` catches what is left, because the generated caller has no unwind table |

### 4.13 The other shape: an opaque host reference (`self: externref`)

§2.7 answered “is a fieldless object still a synthetic object” with *yes, but its
identity collapses*; §4.12 built one. This is the shape for the case that answer
sent elsewhere: the native class whose payload cannot be written in Wasm at all —
an `std::vector`, an `std::string`, a vtable. The guest then gets an **opaque host
reference**, and the generated surface is the same one §4.12 walked.

**The descriptor is one key longer.**

```json
{ "name": "demo.Session", "version": 1, "self": "externref",
  "properties": [ { "name": "status", "type": "i32", "get": "get_status" } ],
  "methods": [ { "name": "send", "argNames": ["bytes"], "params": ["i32"], "results": ["i32"] },
               { "name": "get_status", "params": [], "results": ["i32"] } ],
  "constructors": [ { "name": "open", "argNames": ["slot"], "params": ["i32"] } ] }
```

`"self": "handle"` is the default and means everything §4.12 described.
`"externref"` changes five things, and only those. Adding
`"destructor": "destroy"` as well is the second half of the story: it makes the
object **owned**, which is what the rest of this section and the *borrowed or
owned* table below are about.

| | `self: handle` | `self: externref` |
|---|---|---|
| fields | the canonical layout, offset-checked at load | **refused** — the payload lives in the host |
| size | `layoutHostType` | none: no `_SIZE`, metadata `size: 0` |
| receiver | `int64_t` slot | `type*` — the `externref` payload |
| lifetime | `…_release(slot)` on the host table | the `HostBridge` reference callbacks |
| construction | `<type>_<name>` returns the slot | `<type>_<name>` returns the reference |

The member tables — methods, properties, constructors, `argNames`, every accessor
rule — are **identical**, because none of them was ever about the layout. That is
the point of the shape: one descriptor, two wires, the same `<type>_<name>`
entry-point names in both.

**Borrowed or owned — one key decides.** A `"destructor"` naming one of the
object's own methods changes *who* ends its life, and therefore what the wrapper
is:

| | borrowed (no `destructor`) | owned (`"destructor": "destroy"`) |
|---|---|---|
| the host creates the object | a constructor returns the reference | the same |
| the guest releases it | nobody: the bridge's count is the authority, and the reference dies with the frame | the wrapper's destructor calls the named method |
| C++ wrapper | **copyable**, no destructor work, static factories | **move-only** with a destructor, `reset()` and `release()`; a declared constructor is a C++ constructor |
| C | no lifetime macro | `<MACRO>_DESTRUCTOR` names the call |

That is the case answered in §2.7 applied to a payload the guest can never hold:
if the host exports “make one” and “destroy it”, the descriptor says so, and the
generated wrapper stops pretending to be a borrowed view. Reference counting is
the *borrowed* mode's answer — the `HostBridge` counts the reference for the
engine — and copying is what a borrowed reference does; an owned one is copied
once too many, which is why it does not compile.

**What each target emits.** The generator adds nothing language-specific; it
projects the same members over a receiver that is a reference:

| Target | The opaque projection |
|---|---|
| **C** | the opaque `struct` typedef, one `typedef` per method over `type* self`, the property macros and accessors, then the constructors and members as ordinary declarations over `type*`. No `_handle`, no `_HANDLE_NONE`, no `_h_fn`, no `_release` |
| **C++** | a wrapper holding `type*`: **copyable** and factory-constructed when the reference is borrowed, **move-only** with a destructor that calls the declared lifetime method when it is owned |
| **Rust** | `pub type <type>_ref = *mut <type>;` plus the `_fn` aliases, and a constructor alias returning that pointer |
| **WIT** | the same `resource`. A component resource *is* the component-model spelling of an opaque host object, so the packaging half is unchanged (§4.10) |
| **Kotlin** | `class X(val ref: XRef)` with a companion factory and no `close()`. The reference's spelling is the toolchain's, so `XRef` is a `typealias` the consumer points at it (Kotlin/Wasm: `JsAny`) |

**The host side.** The import's parameter is the reference, so the host function
takes it as the opaque payload:

```cpp
std::int32_t sessionSend(WASM::VMContext* context, void* self, std::int32_t bytes)
{
    auto* session = static_cast<Session*>(self);
    if (session == nullptr)
        WASM::trap(WASM::TrapKind::Internal, "sessionSend: null reference");
    return session->send(bytes);          // the std::vector lives here, not in Wasm
}

void* sessionOpen(WASM::VMContext* context, std::int32_t slot);
// returns the pointer the bridge will count: the constructor's result
```

```cpp
imports.registerFunction("demo", "send", WASM::nativeImport<&sessionSend>());
imports.registerFunction("demo", "open", WASM::nativeImport<&sessionOpen>());
```

The `HostBridge` callbacks from
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §2.2 are what make it safe
rather than lucky: `references.isHostReference` says the pointer is yours,
`retain`/`release` are the engine's count on it, and `instanceDestroying` is where
the last reference an instance was given is dropped.

**The guest side is the generated wrapper and nothing else.**

```cpp
// Borrowed: the host (or the bridge) owns the lifetime; the wrapper copies.
DemoSessionV1 view(referenceHandedOverByTheHost);
const std::int32_t status = view.status();             // the property accessor

// Owned: the descriptor named a lifetime call, so this is one object with one
// destroy — the scope is the object's life, and copying does not compile.
DemoSessionV1 session = DemoSessionV1::open(0);        // or: DemoSessionV1(0)
const std::int32_t sent = session.send(64);            // the import above
anotherCall(session.self());                           // borrow the reference
// ~DemoSessionV1() calls `…_destroy` once, here
```

For the borrowed shape there is nothing to release: a GC guest's reference dies
with its frame and the bridge's count is the authority. For the owned one the
wrapper *is* the owner — `reset()` destroys early, `release()` gives the reference
away without destroying, and a move leaves exactly one owner, which the test suite
runs and counts.

**What this shape is not.** It is not a synthetic object (§2.7): there is no
`TypeId`, no `ref.test`, no canonical layout. It is not a component resource
either until someone packages it — the WIT target is that projection, and
`resource.drop` reaching a host table is still `SO21`. And it does not replace the
handle shape: a guest whose toolchain cannot spell a reference uses `self: handle`
on the *same* descriptor (§16.2 of [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
records which can), which is why both spellings live on one schema.

### 4.14 Where the descriptor comes from

§4.12 and §4.13 both begin with a JSON document. This is the step before: who
writes that document, and why the tool declines to derive it.

**The answer: `objects[]` is authored.** It is the one part of
`wasmjit.binding/2` that has no producer in the tree, and that is a decision
rather than an omission — `SO22` records it as the gap it is:

- **a Wasm module does not carry it.** A module has types and functions; a
  `(struct (mut i32) (mut i32))` has no field *names*, no version, no method
  table, no property accessors and no lifetime policy. Those *are* the descriptor.
- **a candidate front-end does not carry it either.** `--candidates` (SWIG) and
  `--glue-candidates` read a C++ surface and produce **host function imports**:
  the first parameter must be `VMContext*`, the rest must map to scalar or
  reference kinds, and a class is not a function.
- **the two cannot be mixed in one invocation.** `--schema` refuses
  `--candidates`/`--glue-candidates`/WIT, and each of those refuses `--schema`,
  so there is no in-tool merge to lean on.
- **the shape is a product decision**, invisible in a header: whether a class is
  a synthetic object or an opaque reference is §2.7's question, and whether it is
  *owned* is one more key (§4.13).

**What the tool does instead is judge the document.** That is why authoring it is
safe rather than fragile: every claim is checked against the engine, not trusted.

| Where the document comes from | When it is the right route | What the tool adds |
|---|---|---|
| **hand-written JSON** | a handful of objects, or a first prototype — §4.12's example | validates offsets against `layoutHostType`, derives `size`/`offset`/`bytes`, checks names, accessors, `self` and `destructor` |
| **your own emitter** — one small program in your build that prints the document from the same table the host registers | a product with many objects, or objects that follow a class list | exactly the same checks; it cannot tell who wrote the document |
| **SWIG / Glue candidates** | the *function* half only: host imports whose first parameter is `VMContext*` | never emits `objects`; merge the two documents before feeding them in |
| **a component's WIT** | when the object already exists as a component interface | projects `types[]` and function entries (`witOwnership.kind == "resource"`), not `objects` |

The workflow that follows from that table is one sentence: **author once, validate
with `--emit schema`, then emit.** `--emit schema` is not a debug dump — it is the
parse-and-normalise step, and its output is the document worth keeping in the
build, because it carries the offsets the engine derived.

**Step 1 — one document, both shapes.** A product that has a plain-data class and
a `std::vector`-holding session writes both descriptors into one file:

```json
{ "schema": "wasmjit.binding/2", "direction": "all", "module": "demo",
  "objects": [
    { "name": "demo.Widget", "version": 1,
      "fields": [ { "name": "width", "type": "i32", "mutable": true },
                  { "name": "height", "type": "i32" } ],
      "properties": [ { "name": "status", "type": "i32", "get": "get_status" } ],
      "methods": [ { "name": "area", "params": [], "results": ["i32"] },
                   { "name": "get_status", "params": [], "results": ["i32"] } ],
      "constructors": [ { "name": "create", "argNames": ["width", "height"],
                          "params": ["i32", "i32"] } ] },
    { "name": "demo.Session", "version": 1, "self": "externref", "destructor": "destroy",
      "methods": [ { "name": "send", "params": ["i32"], "results": ["i32"] },
                   { "name": "destroy", "params": [], "results": [] } ],
      "constructors": [ { "name": "open", "params": [] } ] } ],
  "imports": [], "exports": [] }
```

Nothing in it is a measurement: `size`, `offset`, `bytes` and `reference` are all
absent, because the reader derives them.

**Step 2 — validate and normalise.** `--emit schema` is the step that turns the
claims into facts:

```bash
wasmjit-bindings --schema objects.binding.json --emit schema > objects.validated.json
```

The output is the same document with `width` at offset 16 and `height` at 20,
`"size": 24`, every type spelled as a `{"type": …}` object, and `self` and
`destructor` preserved where they were declared. Keeping *that* file in the build
is what makes a later emitter change visible.

**Step 3 — the judge refuses the three mistakes that matter.** Each of these is a
message, not a broken header in a consumer's build:

```text
object 'demo.Widget' field 'height': declared offset 12 disagrees with the canonical offset 20
object 'demo.Session': an `externref` object has no fields — the payload stays in the host,
    so the guest reaches it only through the declared accessors
object 'demo.Session' `destructor` 'dispose' is not declared in `methods`; the lifetime call
    is a method of the object like any other
```

**Step 4 — the functions come from a candidate, the objects from you.** A product
that already has a `VMContext*`-shaped host surface lets SWIG or Glue produce the
*function* half, and merges its own descriptors in before the emitting run. There
is no flag for this, deliberately — the merge is ten lines of the build's own
script:

```python
import json
functions = json.load(open("host.functions.json"))   # --candidates … --frontend-schema
objects = json.load(open("objects.binding.json"))    # authored or generated
functions["schema"] = "wasmjit.binding/2"            # objects are the /2 addition
functions["objects"] = objects["objects"]
json.dump(functions, open("combined.binding.json", "w"), indent=2)
```

```bash
wasmjit-bindings --schema combined.binding.json --emit c-header > bindings.h
```

**Step 5 — or let the build print the document.** For a product with many objects,
the emitter is the same three loops every time: walk the class table, print
`fields` from the layout the host already registers, print `methods`, `properties`
and `constructors` from the function table, print one `objects[]` entry per
`(name, version)`. Two rules keep it honest:

- **it must read the same table the host registers**, or the two drift and the
  tool refuses the document (which is the safe failure);
- **it must not print `size` or `offset`.** Those are the tool's half of the
  contract; printing them turns a derived fact into an assertion that will one
  day be wrong.

A generated document is not trusted more than a hand-written one — `--emit schema`
sees the same claims either way. That is the whole reason §4.12's tutorial could
start from JSON without asking where it came from, and why it is worth answering
the question here instead.

**What would change this** is a class-aware candidate front-end: something that
reads a C++ surface and emits `objects[]` itself. It would still have to be *told*
each class's `self` (handle or opaque) and which method destroys it, because those
are product decisions, not facts about the class — so it would be a generator that
takes a policy file, not a parser that infers one. Until such a front-end exists,
the authored document is the input, and `SO22` says so.

## 5. Gaps

Every row below is either a **claim that has not been demonstrated** or a missing
primitive. Revision 2 splits them by owner: `SO13`–`SO16` are the new ones, and
`SO13` is the only engine row. Severity is stated against the scenario (“can a
product ship this?”), not against the engine's completeness meter
([`../SCOPE.md`](../SCOPE.md) §2).

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~SO1~~ | ~~**No handle-lifetime test anywhere.**~~ **Done (SO-3):** `[hostobject][handle]` creates, uses, releases, reuses the slot and then presents the stale handle — and forges the index and the generation beside it. | `HostHandleTable`; [`../../Test/HostObjectTests.cpp`](../../Test/HostObjectTests.cpp) | Closed — §4.9 |
| ~~SO2~~ | ~~**No RAII wrapper is exercised.**~~ **Done (SO-3/SO-4) for C++:** `HostObject` *is* the host-side wrapper — move-only, one release in the destructor — and the handle case makes a refused `acquire` throw after allocation to prove the temporary's count leaves on the unwind. The *guest-side* wrapper is generated too (`emitCppObjects`) and its release count is asserted by a compiled-and-run test. A Rust/Zig wrapper is emitted work, not engine work, and stays open as tooling. | [`../../WasmBase/WasmHostObject.hpp`](../../WasmBase/WasmHostObject.hpp); §4.10 | Closed — §4.9/§4.10 |
| ~~SO3~~ | ~~**No GC-language wrapper test.**~~ **Partly closed (SO-4):** the Kotlin/Wasm façade is emitted and its text is asserted (`close()` guarded, one release, `HANDLE_NONE`); the C++ façade is emitted, compiled and *run* with a release counter; both end at the table's idempotent `release`, which `[hostobject][handle]` exercises twice. No Kotlin toolchain runs in this tree, so that one is text-verified rather than compiled — and a cycle through either wrapper is a host cycle, collected by trial deletion as §4.6 records. | §4.10; `emitKotlinObjects`, `emitCppObjects` | Closed for the emitted façades — §4.10 |
| SO4 | **Component resource parity is untested as a *parity* test.** The destructor-once half is green (`component resource destructor runs in the defining runtime`), but nothing yet makes `resource.drop` *be* a `HostHandleTable::release`, so an `own`/`borrow` on an object cannot be compared with the `i64` path | §4.10; `SO21` | Medium; the parity test is gated on `SO21` |
| SO5 | **No cross-Store identity test.** Two Stores with the same stable type name must yield different `TypeId`s and the same host descriptor, and a handle from one must be rejected by the other | the rule is §3.2.1 and A.14.2; nothing in the tree | Medium; host-side — the test that justifies §3.2.1 |
| ~~SO6~~ | ~~**No ownership accounting test.**~~ **Done (SO-3):** the table's root *is* the engine count the object already had (`liveObjectCount()` does not move on `acquire`), a second `release` is a no-op, and recycling happens only at the safepoint. One object seen by a guest reference and a handle still has one count. | §4.9; `[hostobject][handle]` | Closed — §4.9 |
| SO7 | **No thread-affinity test.** Two agents calling one object without synchronisation must be refused or serialised by a documented policy | `THREADSAFETY.md` covers guest state, not host objects | Medium; host-side |
| ~~SO8~~ | ~~**No negative test.**~~ **Done (SO-3):** null, forged index, forged generation, generation 0, double release, a foreign Store's object and a wrong type are each refused by name, and a dead handle that reaches the guest's import traps with the host's reason. | §4.9; `[hostobject][handle]` | Closed — §4.9 |
| SO9 | **No arena-object test** (Appendix A.13): instance A creates, instance B reads, A is unloaded and re-instantiated and the object survives; a forged offset, a freed slot and a cross-Store tag are clean errors | the arena needs the layout of [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) `SLM-1`…`SLM-4` first | Medium; host-side, gated on the sharing plan |
| SO10 | **No arena-ownership test**: the arena allocator and a host count must never both free one object, and a borrow must not outlive its owner | same gate as SO9 | Medium; host-side |
| ~~SO11~~ | ~~**No descriptor registry and no generator output exists** for the object case.~~ **Done (SO-2):** the descriptor is a typed model (`ObjectField`/`ObjectMethod`/`ObjectDescriptor`), and C, Rust and WIT output is emitted from it (§4.5). | [`../../Tools/Bindings/BindingModel.hpp`](../../Tools/Bindings/BindingModel.hpp), [`../../Tools/Bindings/EmitObjects.hpp`](../../Tools/Bindings/EmitObjects.hpp) | Closed — §4.5 |
| ~~SO12~~ | ~~**No transport for a host object that is not an `externref`** beyond the integer handle~~ **Done (SO-3):** the handle *is* that transport, and it is now measured end to end in a module (`handle_read`/`handle_write` over `i64`, resolved by the host) rather than asserted. `externref` remains the optimisation the wire table describes (§8). | §4.9; `Test/wasm_wat/host_handle_probe.wat` | Closed — §4.9 |
| ~~SO13~~ | ~~**No public way to register a host-originated type.**~~ **Done (SO-1):** `internHostType` builds a `Subtype` and runs `TypeRegistry::internGroup`, the *same* canonicalizer a module uses; `Store::allocateHostStruct`/`allocateHostArray` take a `TypeId` and need no module. | [`../WasmHostType.hpp`](../../WasmBase/WasmHostType.hpp), [`../WasmStore.hpp`](../../WasmBase/WasmStore.hpp) | Closed — §4.4 |
| ~~SO14~~ | ~~**No object descriptor in the binding schema.**~~ **Done (SO-2):** `wasmjit.binding/2` carries an `objects` section; it is validated against the canonical layout at load and consumed by every emitter (§4.5, `EMBEDDING_SURFACE.md` §11.8). | [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §11.8; [`../../Tools/Bindings/BindingModel.cpp`](../../Tools/Bindings/BindingModel.cpp) | Closed — §4.5 |
| ~~SO15~~ | ~~**No dual-surface test.**~~ **Done (SO-3):** one object is reached by a `(ref $pair)` export and by an `i64` handle export in the same run, a write through either is visible through the other, releasing the handle does not free the guest's object, and the guest's drop does not free a handle-rooted one. | §4.9; `[hostobject][handle]` | Closed — §4.9 |
| ~~SO16~~ | ~~**No structural-dedup test for host types.**~~ **Done (SO-1):** `[hostobject]` pins a module-declared `{i32,i32}` and a host definition to one `TypeId`, and a mutability change to a different one — no name involved, which is the evidence for D1. | [`../../Test/HostObjectTests.cpp`](../../Test/HostObjectTests.cpp); [`../TYPE_IDENTITY.md`](../TYPE_IDENTITY.md) §6.2 | Closed — §4.4 |
| ~~SO17~~ | ~~**A host type with a reference field cannot be interned.**~~ **Done (SO-6):** a GC reference field interns and is dropped by the Store's safepoint walk; the remaining restriction is narrower and has its own ID (`SO18`). | `internHostType`; `Store::hostChildSlots`; [`../WasmStore.hpp`](../../WasmBase/WasmStore.hpp) | Closed — §4.6 |
| SO18 | **A reference field whose count is not the Store's cannot be interned.** An `externref` is counted by a host bridge and an `exnref`/`contref` by the exception/continuation machinery; the Store owns neither, so recycling such an object would leave the count behind. | `internHostType` refuses it by name; `layoutHostType` and the `SO-2` schema still describe it | **Documented limit**; `SO-3`'s bridge policy is where it could be lifted |
| SO19 | **A signal the host raises between calls cannot re-enter the guest.** A subscription is a stored callback, and invoking it when no call is in flight crosses the one-agent-per-call contract; a *dynamic* signal name crosses SOD6 as well | §4.8; the `[multiagent]`/`[concurrency]` contract; no callback table with lifetime rules exists | **Documented limit**; the static pattern (§4.8 row 1) covers the product need; the callback table is its own proposal |
| SO20 | **A handle carries no table identity.** Resolving it against a table it did not come from either falls outside that table's slots or — if the index and generation happen to match a live slot — aliases it | §4.9; the mitigation is structural (one table per Store, the handle stays host-side) | **Documented limit**; a per-table salt or tag would make it checkable, and `SO5`'s cross-Store test is where it would show |
| SO21 | **`resource.drop` does not reach a `HostHandleTable`.** The component resource destructor runs once in the defining runtime (proven), but nothing lowers a component-level `release: func(u32)` into a core module and calls the table, so a component cannot yet release a host object through its resource | §4.10; no such fixture or import path exists | **High for `SO-4`'s component half**; needs a component fixture plus the lowered import, not an engine change |
| SO22 | **No producer for `objects[]`.** The descriptor half of `wasmjit.binding/2` is authored — hand-written, or printed by the embedder's own build step — because nothing derives it: a module carries no field names, versions or members; `--candidates`/`--glue-candidates` emit function imports only; and a class's `self` and `destructor` are product decisions rather than facts about the class | §4.14; `--schema` cannot be combined with a candidate or WIT input, so there is no in-tool merge | **Documented limit**; the tool validates whatever arrives (offsets through `layoutHostType`, names, accessors, lifetimes), and a class-aware front-end would need a policy file, not a parser |

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
(additive public API, no removal); `SO-6` is engine work on the same surface;
`SO-2`–`SO-4` are tooling/host work; `SO-5` is host-side work gated on the sharing
study and is **not** an engine milestone; `SO-7` is tooling-only — a section in
`SO-2`'s descriptor and its three emitters, with no engine surface at all.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **SO-1 — Host type registration and allocation** ✅ | A public path to intern a host-originated canonical type into the Store's `TypeRegistry` (the same interning that module types use), and a way to allocate/free an object of that `TypeId` in the Store's `GcHeap` with the header stamped and the count at its documented starting value. No descriptor, no names, no dispatch in `WasmBase/` (A.14.3). | SO13, SO16 | **Done 2026-11-18:** `internHostType` interns through `TypeRegistry::internGroup`; `Store::allocateHostStruct`/`allocateHostArray` allocate from a Store-owned region set; `[hostobject]` proves the module and the host share one `TypeId`, `ref.test`/`struct.get` see the object, and the counts balance across a guest store and a safepoint (§4.4). The name/version half of the original wording is `SO-2`'s descriptor and does not change the type-level test. |
| **SO-2 — The descriptor and the schema** ✅ | A hand-written descriptor table (fields, methods, parent, interfaces, version) and an extension of the neutral envelope with a `types`/`objects` section, consumed by `Tools/Bindings/`. Emit WIT, and at least the C and Rust targets (A.14.5). | SO14, SO11 | **Done 2026-11-18:** one `wasmjit.binding/2` descriptor source validates each field offset against `WASM::layoutHostType` at load and emits a versioned C block (opaque handle, constants, method typedefs, walkable tables), a Rust block and a WIT `resource` per `(name, version)`; the emitted C compiles with `gcc`, the Rust with `rustc` and the WIT parses with `wasm-tools` (§4.5). The name/version half is the emitted table plus the host's `(name, version) -> TypeId` map (`SO-3`), not an engine registry. |
| **SO-3 — The handle table and the dual surface** ✅ | A host-side handle table whose entry holds one `refClone` root, generated `i64` accessors, and the rule that a GC guest may pass the same object as `(ref $T)`. Explicit release, stale-handle rejection, no second count (A.14.4). | SO1, SO2, SO6, SO8, SO12, SO15 | **Done 2026-11-19:** `HostHandle`/`HostHandleTable` in `WasmHostObject.hpp` take the count the object already owns, validate on resolve (generation *and* type), and retire a slot's generation on release; the emitters add `…_handle`, `…_HANDLE_NONE` and one `_h_fn` per method and property. `[hostobject][handle]` reaches one object through a `(ref $pair)` export and an `i64` export in the same run, releases in both orders, refuses seven forged/stale/foreign forms, and traps a dead handle on the guest's call path (§4.9). The one limit it keeps is `SO20`. |
| **SO-4 — Component packaging, GC-language façade** 🟨 | A WIT resource version of the same API, plus the generated wrapper for a GC language (Kotlin/Wasm or MoonBit) and a Dart/Kotlin-style façade holding the handle. | SO3, SO4 | **Partial 2026-11-19:** the world now imports the interface that carries the object resources (parsed by `wasm-tools`), and `emitKotlinObjects` emits one Kotlin/Wasm `AutoCloseable` per object holding the `i64` slot with a single guarded release; the destructor-once half is the engine's green component case. **Not done:** `resource.drop` reaching a `HostHandleTable` — that is `SO21`, and the `own`/`borrow` parity test rides on it (§4.10). |
| **SO-5 — The arena backing** ⛔ | Objects as `i32` offsets in the shared memory of [`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) A.13, gated on `SLM-1`…`SLM-4`. | SO9, SO10 | **Not started 2026-11-19:** the gate is unshipped — `SLM-1` (recipe), `SLM-2` (ownership), `SLM-3` (allocator contract) and `SLM-4` (test set) are all open, so an arena allocator now would be a second allocator contract beside the one the sharing study owns (§4.11). The acceptance case is already written: the object survives the writer's unload and a forged offset is a clean error. |
| **SO-6 — Reference-field graphs on the Store release path** ✅ | Let `internHostType` accept a reference field again: at a safepoint, a released host object drops each outgoing reference (the same `refClone`/`refDrop` contract) before its block is recycled, reusing the instance walker's type/layout logic rather than a second collector. | SO17 | **Done 2026-11-18:** a GC reference field interns; the drain walk drops each reference the object holds, queues a child that reaches zero in the same iterative list, and leaves cycles to the instance's trial-deletion pass. A bridge-owned reference field stays refused (`SO18`). `[hostobject][gc]` pins `gcRefCount` 2/2 → 1/1 → baseline and the empty release queue for a cycle (§4.6). |
| **SO-7 — Accessor-backed properties** ✅ | A `properties[]` entry in the descriptor: a value type plus a required getter and an optional setter, both of which must be methods of the object. Emit the accessor names and signatures in C and Rust and a `func()` member in WIT; take no storage slot and check the declared signature against the property's type. | SO-2 (the schema); nothing engine-side | **Done 2026-11-19:** `demo.Widget` interns three properties with an unchanged `size` and unchanged offsets; ten malformed descriptors are refused at load (no getter, an `offset`, `mutable` without a setter, a field/method name clash, an undeclared accessor, a wrong getter or setter signature, a duplicate property, a shared accessor); the emitted C/Rust compile and the WIT parses with the accessor method projected once (§4.7). |

**`SO-1`, `SO-2`, `SO-3`, `SO-6` and `SO-7` landed, `SO-4` partly.**
The engine seam, the resource walk and the handle table are in the tree
(§4.4/§4.6/§4.9), and so are the tool-side descriptor, its emitters, the
accessor-backed property and the component/GC-language packaging (§4.5/§4.7/§4.10).
`SO-5` (the arena) is the last row, gated on the sharing study. `SO18` and `SO20`
are the documented limits the engine and the table keep, `SO19` is the object
model's (a signal raised between calls cannot re-enter the guest), and `SO21` is
`SO-4`'s residual: the plumbing from a component resource to the handle table.

Ordering, and why it is not negotiable: `SO-1` is the only engine row and
everything else is written against it — generating bindings for a type the engine
cannot intern would produce headers that cannot be tested. `SO-2` before `SO-3`
because a handle table without a descriptor is a table of anonymous slots.
`SO-4` is optional for a core-module product and mandatory for a component one.
`SO-5` is last for the reason revision 1 gave: it depends on a memory-layout plan
that is explicitly out of scope for the short term
([`SHARING_LINEAR_MEMORY.md`](SHARING_LINEAR_MEMORY.md) §12.3, rungs 5–6).
`SO-6` follows `SO-1` directly and is independent of `SO-2`–`SO-5`. `SO-7` needs
`SO-2`'s descriptor and nothing else, which is why it could land on top of the
schema while the handle table (`SO-3`) was still open.

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
five claims the lab never tested, and they are `SO-1`–`SO-6`'s acceptance criteria:

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

**Shipped (`SO-3`, 2026-11-19).** `HostHandle` and `HostHandleTable` in
[`../../WasmBase/WasmHostObject.hpp`](../../WasmBase/WasmHostObject.hpp) implement
this table: `{index, generation}` in one `i64`, `acquire` taking the count a
`HostObject` already owns (no clone), a generation *and* type check on resolve, and
a retired generation that cannot be re-entered. §4.9 has the evidence and the one
limit (`SO20`: a handle carries no table identity).

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
        { "name": "x",   "type": "i32", "offset": 16 },
        { "name": "y",   "type": "i32", "offset": 20 }
      ],
      "properties": [
        { "name": "status", "type": "i32",
          "get": "get_status", "set": "set_status" }
      ],
      "methods": [
        { "name": "resize", "argNames": ["width", "height"],
          "params": ["i32", "i32"], "results": [] },
        { "name": "get_status", "params": [], "results": ["i32"] },
        { "name": "set_status", "params": ["i32"], "results": [] }
      ],
      "constructors": [
        { "name": "create", "argNames": ["width", "height"],
          "params": ["i32", "i32"] }
      ] }
  ],
  "imports": [], "exports": []
}
```

The two offsets are the canonical, header-inclusive ones `layoutHostType` derives
for a `{i32, i32}` struct, and they are optional: a descriptor that states one is
making an assertion, which the loader checks, and a descriptor that omits one is
simply asking the tool to derive it. **Shipped in `SO-2`** — see §4.5 for the
validated model, the five load-time refusals, and the versioned C/Rust/WIT
artifacts.

**A parameter has a name, and the name is the declared one.** `argNames` is the
index-aligned list a *function* entry already carries (§11.8 of
`EMBEDDING_SURFACE.md`), reused verbatim for a method or a constructor: the entry
above does not reach the generator as “two `i32`s” but as `width` and `height`, and
every emitter spells them:

| Target | The call above, as emitted |
|---|---|
| C | `wasmjit_object_demo_widget_v1_handle wasmjit_object_demo_widget_v1_create(int32_t width, int32_t height);` |
| C++ | `explicit DemoWidgetV1(int32_t width, int32_t height)` |
| WIT | `constructor(width: s32, height: s32);` |
| Kotlin | `fun create(width: Int, height: Int): DemoWidgetV1` |
| Rust | `pub type wasmjit_object_demo_widget_v1_create_h_fn = unsafe extern "C" fn(width: i32, height: i32) -> i64;` |

A list shorter than the signature is legal and does not shift the others: the
parameters it does not name keep their positional spelling (`p1`), which is also
what every unnamed parameter gets. Three things are load-time errors, because each
would be a header a consumer cannot compile or a signature whose names collide:
more names than parameters, two parameters of one name (`size` and `Size` lower to
one identifier), and a parameter named `self`, which is the receiver.

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
