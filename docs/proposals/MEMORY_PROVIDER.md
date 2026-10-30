# The Memory Provider — What Runtimes Demand of the Host, What the Engine Already Offers, and Whether to Open the Seam {#prop_memory_provider}

Status: engine-seam study (revision 1 — 2026-10-30). It answers the question the
language screen parked on `RA4` — *"the memory provider is core-chosen, so a
runtime that wants to own its allocator, relocate its heap, or interpose on
`memory.grow` has no seam today"* — by separating the demands that sentence
bundles, measuring what the tree actually does, and pricing each way of changing
it. **Nothing here is implemented**: §7's milestones are proposals, and two of
them are recommended *against*. It answers the six questions it was commissioned
with in §10.

Audience: anyone touching linear memory (`WasmBase/WasmMemory.hpp`,
`WasmMemory.cpp`, `WasmStore.cpp`, `WasmModuleInstance::instantiateMemories`), an
embedder who holds a `LinearMemory*` across a guest call, and the feasibility
charters whose rows cite `F3`/`F8` (`S5`, and the `L7`/`L8`/`L9` questions in
[`../languages/CHARTERS.md`](../languages/CHARTERS.md)).

Follows the same split as [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md) /
[`EXCEPTIONS.md`](EXCEPTIONS.md) / [`GC.md`](GC.md): §2 what is demanded → §3
what is **not** demanded → §4 what is in the tree → §5 gaps → §6 where we may
deviate → §7 plan → §8 test survey → §9 references. §10 is an appendix, in the
shape `GC.md` §10, `STACK_SWITCHING.md` §10 and `LEGACY_EXCEPTIONS.md` §10
already use.

Contains **no new code**. The measurements in §4 were taken on 2026-10-30
(clang 23.1.4, x86-64 Linux) against the built `libWasmBase` — the probe was a
throwaway program outside the tree, not a new test.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What do runtimes actually demand of a linear memory — and which of those is a *host* question? |
| §3 | What does the specification, and what does any candidate language, **not** demand? |
| §4 | What is in the tree today, layer by layer, and what does it measurably do? |
| §5 | Where do we fall short (`MP1`–`MP6`)? |
| §6 | Where may we deliberately stay as we are (`MPD1`–`MPD5`)? |
| §7 | Plan (`MP-0`…`MP-4`) and the verdict per milestone |
| §8 | Test survey — what exists, what each milestone would need |
| §9 | References |
| §10 | The six commissioned questions: conflicts, safety, size, usefulness, workarounds, least resistance |

## 2. What is demanded

`RA4` bundles three requests into one sentence; §2.1 finds five. They are not the
same request, they are not answered by the same change, and only one of them
has both a requester among the candidate languages and a fix that changes the
engine.

### 2.1 The five demands

| # | Demand | What it means | Who asks for it |
|---|---|---|---|
| **D1** | **A guest-owned heap** | the module owns its whole memory; `__heap_base`, `sbrk`, `malloc`, a bump allocator all work because nothing else writes there | every C/C++/Rust/D/Zig-class toolchain — **satisfied today** |
| **D2** | **A base that does not move** | `LinearMemory::memoryBase` is the same pointer before and after a growth, so a host-side holder can keep it | embedders holding a pointer across a guest call; the BYOB/`mappable` sub-proposals ([`MEMORY_CONTROL.md`](MEMORY_CONTROL.md) §2) |
| **D3** | **Reserve now, commit later** | the runtime asks for a large address range and touches few pages of it; commitment (and resident size) follows use | Go (`T09`), .NET NativeAOT (`T14`), GHC's RTS (`T83`) — every runtime that reserves an arena |
| **D4** | **The embedder's own backing store** | the bytes live in a file mapping, a host arena, pinned or device memory; the host gets a window on them | *embedders*, not languages; the three `MEMORY_CONTROL.md` sub-proposals would consume it |
| **D5** | **Observation and policy at growth** | "tell me when it grew", or "refuse this particular growth" | the Emscripten standalone runtime (`T01`), and embedders that want a per-module quota |

The distinction that decides this note: **D1 and D3 are about the guest's
arithmetic and the host's virtual memory; D2, D4 and D5 are about the host's
ownership of the bytes.** `RA4`'s "own its allocator" is D4, "relocate its heap"
is D2/D3, "interpose on `memory.grow`" is D5 — three different seams: a policy
parameter, a factory, and a callback. Only the factory is expensive, and it is
the one nothing asks for.

### 2.2 What each shortlisted runtime names

Read against [`../languages/TRIAGE.md`](../languages/TRIAGE.md); the tier is the
row's own evidence tier, not a new claim.

| Row | Friction it names | Demands | Tier |
|---|---|---|---|
| `T01` C + Emscripten | `F7`+`F3`: a JS-free runtime path (`_start`, memory-growth notification), and "the malloc'd heap base is not the embedder's to move (`RA4`)" | **D5**, **D1** | `E1` |
| `T09` Go | `F2`+`F7`+`F8`: "a large heap reservation against the 2 GiB budget" | **D3** | `E1` |
| `T14` .NET NativeAOT | `F3`+`F8`; the row itself says heap behaviour is "undocumented in what was read" | **D3** (assumed) | `E3` |
| `T39` D | `F3`+`F6`: druntime, a GC and TLS | **D1**, maybe **D3** | `E2` |
| `T83` Haskell | `F2`+`F6`+`F8`: "an RTS with green threads, its own heap" | **D3** | `E2` |

**No shortlisted language names D4.** The provider factory `RA-2` describes is a
demand of *embedders*, and no charter has asked for one. That is the single most
important line in this note, and §7 acts on it.

### 2.3 What the specification requires, and therefore what a change may not break

Conformance, not ergonomics — every option in §7 keeps all four:

- a new memory is **zero-filled**. Both providers get that for free at
  allocation; a lazy provider must get it at commit rather than by `memset`
  (`mmap` and `VirtualAlloc(MEM_COMMIT)` both hand back zeroed pages on our
  targets, which is why `SharedLinearMemory` commits without a `memset`);
- `memory.grow` is **all-or-nothing**: it returns the previous size and the new
  pages are usable, or it returns −1 and nothing changed — including the
  accounting;
- `memory.size` must observe another host thread's growth (the seq-cst fence in
  `Store::growMemory`, `THREADS.md` TH-3/T7);
- a byte above `memoryMax` is out of bounds even when the host could back it.

## 3. What is not demanded

This is the half that shrinks the problem.

### 3.1 The specification has no opinion about the backing store

`MemoryType` is an index type, limits, a `shared` flag and a page size; a memory
*instance* is an array of bytes. Where those bytes come from is not observable
from inside the module — a guest cannot read its own host address, and no
instruction distinguishes an `mmap` from a `malloc`. So **D2, D4 and D5 lie
entirely outside conformance**: an engine may satisfy them, refuse them or ignore
them and conform either way. That is why §7 prices them in embedder value rather
than in spec compliance.

### 3.2 No language asks for the host's allocator

A C++ module's `new`, a Rust module's `dlmalloc`, AssemblyScript's TLSF heap and
D's GC all live *inside* linear memory and are written by the guest. The host
allocator is invisible to them; a runtime that "wants its own allocator" in the
`RA4` sense wants **its own address range** (D3), not the engine's `malloc`.

### 3.3 Growth notification is the embedder's job, by construction

Emscripten's standalone output exports `emscripten_notify_memory_growth`; the
host is *supposed* to call it after a growth, and the module re-derives its
views. That is a call the embedder makes into the guest, not a hook the engine
owes. All the embedder needs is a way to notice a growth — and
`wasm_memory_size()` already returns the current size, so compare it, or call the
export after any call that might grow. §10.5 records this as a workaround that
works today.

### 3.4 A new memory *kind* is not needed for any of it

D3 changes how the bytes of an existing memory are obtained. It needs no new
opcode, no new `MemoryType` flag, no new import kind and no second provider class
— it needs the *existing* private memory to be allowed to reserve. Anything that
adds guest-visible surface to solve a host-side problem is over-building.

### 3.5 What MC4's guard pages are not

`MEMORY_CONTROL.md` MC4's "static memory protection" is a **guest-visible
feature** — an unmapped null page at the start of the address space, so a null
dereference traps. It would want the same machinery as D4 one day, but it is a
proposal about module semantics, not an allocation policy, and this note does not
implement it.

## 4. What is in the tree

Linear memory has **three layers**, and the seam is closer to existing than `RA4`
suggests.

### 4.1 The view (`LinearMemory`)

A standard-layout POD whose field offsets are the backend ABI: `memoryBase`,
`memorySize`, `memoryMax`, `isShared`, `hostData`, `is64`, `pageSize`. Generated
code addresses bytes as `memoryBase + offset`, so the view is the contact point
with every backend and its layout cannot change (`ABI.md`).

### 4.2 The provider (`StoreOwnedMemory`) — already an interface

`StoreOwnedMemory` is an **exported abstract class**: it owns a `LinearMemory`,
hands it out by `getMemory()`, declares `growMemory(deltaPages)` pure virtual, and
offers `discardHint(offset, bytes)` virtual with a portable default (return false
→ the caller zero-fills). That is exactly the shape of a provider interface, and
it is what makes D2/D3/D4 implementable *without touching the backend contract*.
What a third provider must satisfy:

1. produce a zero-filled memory of the declared initial bytes;
2. `growMemory` honours `memoryMax`, is all-or-nothing, zero-fills the new pages,
   and publishes `memorySize` (and `memoryBase`, if it moved) before returning —
   `Store::growMemory` adds the seq-cst fence afterwards;
3. a memory created with `isShared` must keep its base stable (host threads hold
   the raw pointer);
4. leave `hostData == this`, because `Store::growMemory` recovers the owner from
   it;
5. never charge the allocation budget — the `Store` does that before calling.

### 4.3 The factory (`Store::createLinearMemory`) — and the one thing that is missing

`createLinearMemory` is **not virtual**, the two concrete providers are `final`,
and there is exactly **one production call site**
(`WasmModuleInstance::instantiateMemories`), which derives `isShared` from the
module's own `MemoryType.limits.flags & 0x02` and `maxPages` from its limits. So
the module decides how its memory is backed, and the embedder has no say at all.
That is `RA4`, stated precisely: *not* "there is no provider contract" but "the
contract exists and cannot be installed".

`Store` has a public default constructor and is not `final`, so a class that
derives from it is legal — and useless, because `memories` is private and the
factory is not virtual.

### 4.4 What the two providers measurably do

Probe run 2026-10-30 against the built library; the numbers are the point.

| Probe | Result |
|---|---|
| private, 1 page, then 8× `growMemory(+1)` | the base **changed on 4 of the 8 grows** — `0x558b…39e0` → `0x7f4b…e010` → `0x7f4b…d010` → (unchanged) → `0x7f4b…c010` → (unchanged three times) → `0x7f4b…b010`; five distinct bases over the run, i.e. it moves exactly when the vector runs out of capacity; size 65 536 → 589 824 |
| private, accounting | charged 589 824 = exactly the 9 pages committed; nothing reserved |
| shared, 1 page, then `growMemory(+1)` | base **stable** — `0x7f4a…b000` before and after; size 65 536 → 131 072 |
| shared, declared max 8 GiB (131 072 pages) | `memoryMax` becomes **4 294 967 296** (4 GiB) |
| private, declared max 8 GiB | `memoryMax` stays **8 589 934 592** (8 GiB) |
| shared, reservation vs. budget | with a 4 GiB reservation live, total charged across both memories is 11 pages (720 896 bytes) = the 11 pages committed |

Three consequences:

- **D2 is unavailable for ordinary memories.** A `memory.grow` on a non-shared
  memory may relocate and copy the buffer (4 of 8 grows in the run above), and
  which grow it will be is not predictable from outside, so any embedder that
  keeps `memoryBase` across a guest call holds a stale pointer. `PrivateLinearMemory`'s
  own comment ("realloc is fine — nobody else holds `memoryBase` concurrently")
  is true of *threads* and false of *the embedder*.
- **D3 is reachable only through `isShared`** — a property of the module's type,
  not a policy. To get reservation, a module must declare its memory shared,
  which also makes it atomics-eligible, obliges the module to declare a `max`,
  and caps `memoryMax` at 4 GiB.
- **The budget charges commits, not reservations** (§4.5), which means `T09`'s
  `F8` sentence needs a correction: the 2 GiB budget is not what forbids a
  reservation — nothing forbids it, it simply is not expressible. What the budget
  bites is the committed heap, and that is every runtime's footprint.

### 4.5 The accounting model, since three of the five demands interact with it

`Store::reserveBytes` charges **bytes at the moment they become usable**: the
initial size at creation, each `deltaBytes` immediately before the provider
attempts the grow (and credited back if it fails). Address space that is only
reserved is never charged. What follows from that:

- a 4 GiB reservation is free, a 4 GiB heap is not — which is the correct
  default for `SAFETY.md` §2.3 (the budget bounds *commitment*, the thing that
  can take the host down), and it is why a lazy provider would not even need a
  new budget concept;
- a provider that copies on grow (as `PrivateLinearMemory` does) briefly holds
  both buffers while being charged for one. At the budget boundary that means a
  grow can be refused even though the new size alone fits — an `MP6` gap, and a
  second reason a reserve-then-commit provider is *safer*, not just cheaper;
- `WASMJIT_MAX_TOTAL_BYTES` is process-wide, not per-`Store` and not per-memory:
  there is no way for an embedder to say "this module gets 64 MiB".

### 4.6 One documentation defect worth naming

`LinearMemory::hostData` is documented as *"Opaque pointer for any external user
data"*. It is not: it is the `StoreOwnedMemory*` back-pointer, and
`Store::growMemory` casts it back on every grow. An embedder that takes the
comment at its word corrupts growth. No seam can be designed on top of that
sentence until it is corrected (`MP4`).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| MP1 | **No way to install a provider.** The contract exists; the factory is `Store`'s and takes no policy. | `WasmStore.cpp` (`createLinearMemory`), `WasmModuleInstance.cpp` (single call site), `READINESS_AUDIT.md` `RA4` | Medium (only if an embedder asks) |
| MP2 | **No reserve-then-commit for a non-shared memory.** Reservation exists, but only behind `isShared`. | `WasmMemory.hpp` (`PrivateLinearMemory` / `SharedLinearMemory`), `MEMORY_CONTROL.md` §2 "Lazy commit (TODO)" | Medium (three rows cite it) |
| MP3 | **A shared reservation rewrites `memoryMax` to `min(max, 4 GiB)`**, silently. A module that declares an 8 GiB max grows no further than 4 GiB, and an *unbounded* shared memory reserves 4 GiB of address space per memory. | measured (§4.4); `MAX_SHARED_BYTES` in `WasmMemory.cpp` | Low (legal, since `memory.grow` may always fail — but surprising) |
| MP4 | **`hostData`'s comment contradicts its use.** | `WasmMemory.hpp`; `Store::growMemory` | Low (documentation, but it is a trap) |
| MP5 | **No growth hook and no per-memory quota.** One process-wide budget; observation by polling `wasm_memory_size()`. | `WasmStore.hpp` (`allocationBudgetBytes`), `SAFETY.md` §2.3 | Low (workaround exists, §10.5) |
| MP6 | **A growing private memory is charged for one buffer while holding two**, so a grow can fail at the budget boundary that would fit. | `PrivateLinearMemory::growMemory` (`backing.resize`), `Store::growMemory` (charge first, copy inside) | Low (bounds worse than the plan, never better) |

## 6. Where we may deviate

### 6.1 The deviations

- **MPD1 — the view is a POD with an owner back-pointer in it.** `hostData` is
  the engine's, not the embedder's. Keep it: the alternative (a side table from
  `LinearMemory*` to owner) costs a lookup on every grow and buys nothing, since
  a provider is reached through the class that owns the view anyway.
- **MPD2 — `isShared` doubles as "reserve up front".** The flag is a *type*
  property (threads) doing duty as an *allocation policy*. Keep the conflation
  for the shared case, but do not extend it: the policy should be a separate
  argument (`MP-1`), or a non-shared memory will never be able to reserve.
- **MPD3 — 4 GiB is the reservation cap.** It predates memory64 and threads
  meeting in one module. Keep until a module needs more; the cap is legal because
  a failed `memory.grow` is always permitted. What must change is that it is
  *silent* (`MP3`).
- **MPD4 — the budget is process-wide and byte-based.** Deliberately not
  per-memory: `SAFETY.md`'s model is "one module must not take the host down",
  and a per-memory quota invites an embedder to think it is a security boundary.
  Reject `MP-4`.
- **MPD5 — a private memory may relocate.** Legal (a guest sees offsets, not
  host addresses) but it is the one deviation with an embedder-visible
  consequence, so it must be documented rather than merely true.

### 6.2 The deviation table

| # | Deviation | Why | Cost if it is wrong |
|---|---|---|---|
| MPD1 | `hostData` belongs to the engine | no lookup on the grow path; stable ABI | an embedder that stores its own pointer there breaks growth — fixed by documentation |
| MPD2 | `isShared` implies reservation | the shared provider predates any non-shared need | reservation stays unreachable without `MP-1` |
| MPD3 | 4 GiB reservation cap | `MAX_SHARED_BYTES`, pre-memory64 | a >4 GiB shared memory cannot grow past 4 GiB; recorded, not silent (§7 `MP-0`) |
| MPD4 | one process-wide budget | `SAFETY.md` §2.3 | no per-module quota; an embedder with several tenants cannot isolate them |
| MPD5 | private memory relocates on grow | `std::vector` — the simplest correct provider | stale `memoryBase` in embedder code; documented, and `MP-1` offers the stable alternative |

### 6.3 Observable semantics we must not play loose with

Independent of which provider is installed, these stay true and are what §8
tests:

1. a fresh memory reads as zeros everywhere;
2. a failed grow changes neither size nor accounting;
3. a successful grow makes exactly `deltaPages` new zero pages readable;
4. `memory.size` from a host thread observes a growth that completed on another;
5. `memoryBase` is valid and non-null for every size > 0, and is the pointer the
   backend will use on the next instruction;
6. the budget is charged once per committed byte and credited on every failure
   path.

## 7. Plan

Five milestones. The two that would be built (`MP-1`, `MP-2`) are **additive**
(`API_ABI_STABILITY.md` §4.1 class A); `MP-0` changes no surface; `MP-3` and
`MP-4` are verdicts, not work.

| Milestone | What | Closes | Status |
|---|---|---|---|
| **MP-0 — Correct the record** | Fix `hostData`'s comment; document that a private memory may relocate and that a shared one caps `memoryMax` at 4 GiB; point `RA4`/`RA-2` at this note and `T09`'s `F8` at §4.5. | MP3 (part), MP4, MPD5 | Proposed — docs only |
| **MP-1 — Make the policy expressible** | One **overload** of `createLinearMemory` taking an allocation policy — *growable buffer* (today's behaviour, the default) or *reserved range, commit on grow* — implemented by reusing the `SharedLinearMemory` machinery for a non-shared memory. It must be an overload, not a sixth defaulted parameter: that would change the mangled name and be class B (§10.3). | MP2, MP6, D2, D3 | Proposed — the only engine change this note argues for |
| **MP-2 — Install a provider** | `Store` accepts a provider (`RA-2`'s "factory/key"): an embedder-supplied `StoreOwnedMemory` factory consulted by `createLinearMemory`. | MP1, D4, D5-quota | **Deferred** — no language and no charter has asked |
| **MP-3 — Notify on growth** | A callback into the embedder on a successful grow. | MP5 (part) | **Rejected** — §10.5's workaround is sufficient |
| **MP-4 — Per-memory quota** | An embedder-settable per-memory maximum below the declaration's. | MP5 (part) | **Rejected** — MPD4 |

### 7.1 Why `MP-1` and not `MP-2`

Because the language-side demand is D3 and `MP-1` meets it in one branch of an
existing function, while `MP-2` is the seam for D4 — a demand nothing in
`docs/languages/` makes. Doing `MP-2` first would buy the more expensive,
more invasive seam to satisfy the cheaper need, and it would make the *provider
itself* an embedder-visible type (its growth contract becomes an ABI promise).

Order matters in the other direction too: if `MP-2` ever lands, `MP-1`'s parameter
should remain the *default* policy, so a provider is an override and not a
prerequisite.

### 7.2 Stop conditions

- `MP-1` stops being worth it if a PoC shows the runtime grows its memory
  gradually and never relies on lazy commit. Then the reservation is a
  footprint optimisation with no correctness value, and the rows citing `F8`
  should be re-graded *down*, not the engine changed.
- `MP-2` becomes worth it the moment an embedder says "I need the bytes in my
  arena", or the day `MEMORY_CONTROL.md` MC4/MC5 get a design — whichever is
  first. Not before.
- Neither milestone may be signed off without the §8 checks, because both move
  the point where bytes become committed, and that is where `SAFETY.md`'s budget
  and the zero-fill guarantee meet.

### 7.3 What this does not do to the language screen

It changes no tier, no cluster and no verdict. At most it moves `T09`/`T83`'s
`F8` from "engine limitation" to "one additive parameter, still to be shown
necessary" — and only once §7.2's stop condition has been answered by a PoC. In
the screen's own terms this note is evidence for `F8`'s *cost*, not for any
language's *feasibility*.

## 8. Test survey

### 8.1 What exists

Memory is covered **behaviourally** — through `memory.grow` in WAT modules — and
not at all as a *provider*:

| Test | What it pins |
|---|---|
| `memory_buffer stores and loads data correctly` | the POD view is what the JIT addresses |
| `imported memory is shared between two module instances` | one `LinearMemory*` per `(module, field)` |
| `multi-memory: two linear memories are independent`, `… memory.copy moves bytes between two memories`, `… imported memories are shared and stay distinct` | the `MemIdx` plumbing, `MM<n>` |
| `memory64: 64-bit memory and table size/grow are 64-bit` | `is64` beyond 32 bits |
| `grow guardrail rejects absurd deltas without allocating` (`wasm_wat/grow_guard.wat`) | the budget refuses an absurd grow instead of allocating |
| `memory.discard zeroes whole pages, preserves the rest, and traps out of bounds` (`wasm_wat/memory_discard.wat`) | `discardHint`'s portable path |
| `custom page sizes: one-byte pages size and grow are byte-exact` | `pageSize` in the grow arithmetic |
| `Test/shared_memory_*.wat`, `profile_shared_memory.wat` | shared memories load and grow |

**Gap in the survey itself:** no test constructs `PrivateLinearMemory` or
`SharedLinearMemory` directly, so the three properties §4.4 measured — base
relocation, reservation-vs-budget, the 4 GiB cap — are true but untested. They
would regress silently.

### 8.2 What each milestone needs

| Milestone | Tests |
|---|---|
| `MP-0` | none (comments); `[headerdoc]` must stay green |
| `MP-1` | a new `[mem-provider]` case: (a) the default policy still relocates and still charges commits only; (b) the reserved policy gives a **stable base** across grows and reports the same `memoryMax`; (c) a reserved grow is charged at commit, so `allocatedBytes()` tracks committed pages, not the reservation; (d) a grow past `memoryMax` fails with no accounting change; (e) new pages read as zeros in both policies; (f) the 2 GiB-budget case from `grow_guard.wat` still traps the same way |
| `MP-2` | (a) an installed provider is consulted for every memory the store creates; (b) a provider that returns `false` from `growMemory` yields a −1 `memory.grow` and a credited budget; (c) a provider that returns `true` without publishing `memorySize` is the embedder's bug — the test documents that this is *not* defended against (§10.2) |
| `MP-3`, `MP-4` | none — rejected |

`MP-1`'s (c) is the one that would have caught §4.5's subtlety, and it is why the
budget interaction, not the base pointer, is the first thing to write.

## 9. References

- The memory section of the core specification — limits, `shared`, page size,
  `memory.grow`/`memory.size`, and the sentence that makes all of §3.1 true: a
  memory instance is a byte array with no observable origin.
- [`MEMORY64.md`](MEMORY64.md) `M<n>` — `is64`, and why `memoryMax` can be
  `UINT64_MAX`.
- [`MULTI_MEMORY.md`](MULTI_MEMORY.md) `MM<n>` — several memories per module,
  each with its own provider instance.
- [`THREADS.md`](THREADS.md) TH-3/T7 — the publish fence and the acquire load
  `wasm_memory_size()` performs; the reason a provider must publish `memorySize`
  before returning.
- [`MEMORY_CONTROL.md`](MEMORY_CONTROL.md) §2 ("Lazy commit (TODO)"), `MC-3`
  (`discardHint`), `MC4`/`MC5` — the prior art for host-specific policy living in
  the provider, and the two sub-proposals that would consume `MP-2`.
- [`CUSTOM_PAGE_SIZES.md`](CUSTOM_PAGE_SIZES.md) `CPS-2` — `pageSize` is a field
  of the view, so every provider divides by it.
- [`../ABI.md`](../ABI.md) "Linear memory ownership" — memories are store-owned,
  import sharing is pointer identity, `hostData` is the back-pointer.
- [`../SAFETY.md`](../SAFETY.md) §2.3 — the allocation budget, and why a
  reservation is free while a commit is not.
- [`../READINESS_AUDIT.md`](../READINESS_AUDIT.md) `RA4` (the limitation this note
  prices) and `RA-2` (the milestone this note splits into `MP-1`/`MP-2`).
- [`../languages/TRIAGE.md`](../languages/TRIAGE.md) `F3`/`F8` and rows
  `T01`/`T09`/`T14`/`T39`/`T83`; [`../languages/CHARTERS.md`](../languages/CHARTERS.md)
  `S5`, whose probe list carries the memory question.
- [`LEGACY_EXCEPTIONS.md`](LEGACY_EXCEPTIONS.md) — the note this one copies its
  shape from, including the appendix of commissioned questions.

## 10. Appendix — the six commissioned questions

### 10.1 What conflicts with what we already promise?

Two that a build would hit, plus one that a *deferral* avoids:

- **`hostData` is spoken for** (MPD1/`MP4`). A provider seam cannot hand the
  embedder that field without moving the back-pointer into a side table, which
  costs a lookup on every grow. Conclusion: the field stays the engine's, and the
  comment gets fixed.
- **`isShared` is doing two jobs** (MPD2). A non-shared memory cannot reserve
  today because the only reserving provider is the shared one. `MP-1` adds the
  policy as an argument instead of a flag, so no `MemoryType` semantics change
  and no guest-visible byte changes.

`MP-2` conflicts with something subtler: a provider contract that embedders can
implement becomes an ABI promise (`API_ABI_STABILITY.md`), and this engine has
been careful to promise only what it can keep. That is the main argument for
deferring it.

### 10.2 Does any of this weaken `SAFETY.md`?

`MP-1`: no. The `Store` keeps the budget (`reserveBytes` before, `releaseBytes`
on failure), keeps the fence, and keeps the zero-fill guarantee — the new path
adds `mprotect`-commit, whose pages are zeroed by the kernel exactly as
`SharedLinearMemory`'s already are. A reservation that is never committed costs
address space, capped at 4 GiB by `MAX_SHARED_BYTES`.

`MP-2`: **yes, one invariant moves out of the engine.** Today "growMemory returned
true" means the pages exist and read as zeros, because the engine wrote both
implementations. With an embedder-installed provider it becomes a promise the
embedder can break, and breaking it means a guest store lands in unmapped memory
— a host signal, i.e. the `std::abort()` class of failure `SAFETY.md` §4 exists
to avoid. That is a real cost, and it is why `MP-2` is deferred rather than
merely unscheduled: it needs a decision about whether a provider is *trusted*.

`MP-3`: would run embedder code on the growth path, where an exception cannot
propagate. Rejected for the same reason.

Trap-versus-abort is untouched throughout: an out-of-bounds access is checked
against `memorySize` and traps, whatever the host has reserved.

### 10.3 How big is it?

- `MP-0`: an afternoon, no code.
- `MP-1`: an allocation-policy parameter plus a non-shared reserving provider;
  ~1 day of engine work and ~1 day of tests (§8.2). **One ABI caveat**: adding a
  defaulted parameter to `createLinearMemory` *changes its mangled name* and
  would show up as one removed plus one added symbol in `abi-check.sh` — i.e.
  class B. It must be done as an **overload** (the existing five-argument
  signature forwarding to the new one) to stay class A.
- `MP-2`: a `MemoryProvider` interface, a `Store` slot, a branch in the factory;
  ~2 days, one new exported class, and the ABI promise discussed above.

### 10.4 Is it worth it?

Split by demand, because that is the whole point of §2:

| Demand | Who wants it | Worth it? |
|---|---|---|
| **D1** guest-owned heap | every toolchain | already there; nothing to do |
| **D2** stable base | embedders (BYOB, host windows); falls out of `MP-1` | yes, as a side-effect |
| **D3** reserve-then-commit | `T09`, `T14`, `T83`; and it is a **footprint** demand, not a correctness one | yes — `MP-1`, after a PoC confirms the need |
| **D4** embedder's arena | embedders only; `MC4`/`MC5` would consume it | **not yet** — `MP-2` deferred |
| **D5** growth hook | `T01`, quotas | no — the workaround is a two-line poll (§10.5) |

The honest summary: **one small change is worth making, one large change is worth
designing but not building, and two are worth refusing in writing.** Nothing here
makes a language feasible or infeasible; it prices a friction the screen already
named.

### 10.5 What works today, without touching the engine?

| Want | Workaround | Cost |
|---|---|---|
| a base that does not move, and lazy commit | declare the memory `shared` with an explicit `max` — `SharedLinearMemory` reserves and commits on grow, base stable | the memory becomes atomics-eligible and capped at 4 GiB; a module you did not write may not allow it |
| notice that memory grew | compare `wasm_memory_size()` before/after a call, or call the toolchain's own export (`emscripten_notify_memory_growth`) | one call per boundary; correct for every toolchain that has such an export |
| a bigger heap | raise `WASMJIT_MAX_TOTAL_BYTES` (`SAFETY.md` §2.3) | global, and it weakens the guardrail for everything in the process |
| a per-module limit | none — the budget is process-wide and static | separate processes |
| bytes in your own arena | **none** — D4 is genuinely unserved | `MP-2`, when someone asks |

### 10.6 Least resistance, in one line

Correct the record today (`MP-0`); let a non-shared memory reserve as an
**overload** with a default that keeps every existing caller byte-identical when
a PoC shows a runtime needs it (`MP-1`, and §7.2 says what would cancel it);
leave the provider factory unbuilt (`MP-2`) until an embedder asks or `MC4`/`MC5`
arrive; and write down the two rejections (`MP-3`, `MP-4`) so the next reader
does not re-derive them.

