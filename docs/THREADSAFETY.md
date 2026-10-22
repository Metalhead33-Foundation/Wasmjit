# Thread Safety — Guest, Host, and the Boundary Between Them

Status: policy / reference note (revision 3 — records the **one instance per
module** architectural target, the host C++ lock caveats, the open guest-lock
question, the trap/lock ownership questions that a future multi-agent build
must answer, and the Go-style trap-cleanup candidate. It contains **no new code**
and changes no runtime behaviour.)

Audience: embedders, Wasm module/toolchain authors, and anyone changing
`WasmBase/`, a backend, the GC heap, the trap path, or a native-import helper.

This note is not a WebAssembly proposal note. It is the umbrella contract that
the proposal notes [`proposals/THREADS.md`](proposals/THREADS.md),
[`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md)
and [`proposals/ACQUIRE_RELEASE_ATOMICS.md`](proposals/ACQUIRE_RELEASE_ATOMICS.md)
assume, and the thread-safety companion to [`SAFETY.md`](SAFETY.md). Where those
notes say what a proposal requires or what `WasmJit` implements, this note says
**who is responsible for which safety property across which boundary**.

The short version:

> **Host-side safety is an engine/embedder contract. Guest-side safety is a
> module/execution contract. Neither one implies the other, and `WasmJit` today
> provides only a subset of each.**

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What exactly do “thread-safe”, “guest”, and “host” mean here? |
| §3 | The two independent axes and their overlap |
| §4 | Guest-side / intra-WASM thread safety |
| §5 | Host-side / embedder-side thread safety |
| §6 | Intersections, non-implications, and common false inferences |
| §7 | Responsibility matrix: engine, module author, embedder |
| §8 | What the current implementation actually guarantees |
| §9 | The embedder's minimum protocol |
| §10 | The future one-instance multi-agent contract, trap/lock ownership, and cancellation |
| §11 | References |

## 2. Terms used precisely

**Thread safety** below means: concurrent operations have no C++/Wasm data
races, no torn reads or writes, no use-after-free or lifetime race, no engine
deadlock, and an observable result permitted by the documented API. It does not
mean “the operation is meaningful” or “the guest's own logic is correct”.

- **Module** — the decoded, immutable artifact (`WASM::Module`): sections,
  code bytes and canonical type identities. A module is not running code.
- **Instance** — `WASM::ModuleInstance`: the mutable execution state — globals,
  memory/table index spaces, GC regions, data/element state, `VMContext` and
  compiled callables. “The module's thread safety” is shorthand for the safety
  of operations on its instances and their shared objects.
- **Agent** — one Wasm evaluation context and stack (the WebAssembly threads
  model's term; informally a Wasm thread). Today there is exactly one agent per
  host thread, and it is the caller's host thread.
- **Host / embedder** — the native C++ process that parses, instantiates and
  calls into Wasm, and supplies native imports.
- **Module author / toolchain author** — the producer of the Wasm artifact and
  the documentation that says which concurrency model (G-A…G-D) its exports and
  imports assume.
- **Call** — one entry into an exported or internal function. A call owns an
  operand stack and a trap context; it does not own the instance.
- **Guest-side** — properties enforced or relied on **inside Wasm execution**:
  guest data races, atomicity, ordering, shared-memory/GC semantics, and
  concurrent agent behaviour.
- **Host-side** — properties of the **C++ API and native callbacks**: engine
  objects used by host threads, engine→native imports, and host→exported Wasm
  calls.
- **Boundary** — an export (host → Wasm) or an import (Wasm → host). Both
  directions cross a native ABI and both inherit the weaker side's obligations.

The distinction matters because “thread-safe” is routinely used for three
different claims:

1. the engine's **internal data structures** survive concurrent host use;
2. a **guest program** has no data races under the Wasm memory model;
3. a particular **function or callback** may be entered concurrently without
   violating its own contract.

`WasmJit` currently makes (1) partially true, (2) mostly a module-author
property, and (3) never inferable from a Wasm type.

## 3. The two axes

Thread safety here is two independent axes, not one spectrum:

| Axis | Question | Examples |
|---|---|---|
| **Who initiates the operation?** | Host threads, or Wasm agents? | `callCallable` from C++; `call` inside Wasm |
| **What state does it touch?** | Private instance state, Store-shared state, or host process state | unshared memory; shared memory/table/GC; `hostData` |

The overlap is the four boundary combinations:

| Initiator | Callee/state | Name in this note |
|---|---|---|
| host thread | exported Wasm function | **H→W export** |
| Wasm agent | native C/C++ import | **W→H import** |
| Wasm agent | another Wasm function/instance | **W→W call** |
| host thread | engine API / Store object | **H→E API** |

A program can be safe on one axis and unsafe on another. The rest of this note
keeps the axes separate instead of collapsing them into a single “thread-safe”
flag.

## 4. Guest-side / intra-WASM thread safety

### 4.1 A module is not “thread-safe”; executions are

A `Module` is immutable decoded data. Thread safety is a property of an
**instance** (its mutable state) and of the **calls/agents** that touch it.
The intended runtime model is deliberately **one `ModuleInstance` per
`Module`**, one `VMContext` per instance, and no per-thread duplicate
instances. The current API and test harness may still instantiate a module more
than once (for isolation tests and spec fixtures), but that multiplicity is
**not** the concurrency plan and must not be used as a thread-safety strategy.
Concurrency has to be handled **around or inside the single instance**.

There are three guest-side execution shapes for that single instance:

1. **One agent, serial calls.** The ordinary case. No guest concurrency exists;
   only the host's own threads matter.
2. **One instance, several host calls serialized by a host/engine lock.** The
   only thread-safe host model available today is external serialization; the
   target is an instance-level synchronization mechanism rather than more
   instances.
3. **One instance, several concurrent agents or host threads.** This is the
   multi-agent target. It needs shared instance state to be made safe: shared
   memory/tables/globals/GC plus atomics and ordering, and/or an engine-owned
   per-instance concurrency protocol. It is **not implemented today**: there is
   no scheduler and one instance is not concurrently callable.

### 4.2 What “guest-threadsafe” must mean

A Wasm instance/function is guest-threadsafe only relative to a stated
execution model:

- **G-A — serial instance.** At most one call enters the instance at a time.
  This is the current engine's model. It makes ordinary non-atomic Wasm code
  valid; it says nothing about concurrent calls.
- **G-B — single instance with serialized entry.** Several host threads may
  exist, but every entry into the instance is serialized by an external lock (or
  by a future engine-owned instance lock). Each call still sees the one
  instance's unshared memory/globals/GC state. This is the current practical
  host model; the lock is outside the Wasm code.
- **G-C — shared-memory agents.** Several agents execute concurrently in the
  one instance and share memory (and, when implemented, shared tables/globals/
  GC). Every access to shared mutable data must follow the Wasm memory model:
  atomic instructions for data races, acquire/release or seqcst ordering as
  required, and no non-atomic access racing with an atomic or another non-atomic
  access. Private/unshared instance state is not a concurrency primitive.
- **G-D — shared GC/table agents.** The Shared-Everything Threads model. The
  static `shared` annotations restrict what may be referred to; the atomics
  provide tear-free access; the GC/refcount barriers and waitqueues are part of
  the runtime's future obligation. Current status: annotations and the
  working-draft atomics decode/execute single-agent, but the GC heap is still
  single-mutator and reference-slot atomics do not yet run the full RC barrier
  (`proposals/SHARED_EVERYTHING_THREADS.md` SET-2/SET-3).

A module author who claims “guest-threadsafe” should state **which** of
G-A…G-D applies. “No unsynchronised shared state” under G-C/G-D is a
module-author proof obligation; the engine cannot validate it and does not
attempt to. Separate instances are not a fourth model: they are outside the
intended runtime architecture and are not a thread-safety answer.

### 4.3 The three guest-side call directions

- **W→W internal calls** (`call`, `call_indirect`, `call_ref`, tail calls).
  These execute in the caller agent's stack and the callee's instance context.
  They have no independent thread-safety property: they are as safe as the
  execution model and the state they touch. Local variables and the operand
  stack are per-call; globals, memories, tables and GC objects are not.
  Consequently a module cannot declare one internal helper “threadsafe” while
  an exported entry point uses it under a different model: every function
  reachable from a concurrent export must obey the same G-A…G-D contract.
- **W→H imports** (Wasm calls a registered native function). The callee is host
  code. Its safety is the embedder's responsibility, not the module's, and the
  engine's call mechanism does not serialize it (§5.3). A Wasm module that calls
  an import from several agents is sharing that import's state by definition
  unless the import is stateless or internally synchronized.
- **H→W exports** (the embedder calls a Wasm export). The exported function's
  safety is a **contract supplied by the module author**; the Wasm function type
  has no thread-safety annotation. Two host threads calling the same export on
  the same instance are two agents entering shared instance state. The engine
  does not add a mutex around this call. The host must serialize the single
  instance, or use a module whose G-C/G-D contract explicitly covers it (and a
  runtime that actually supports agents). Creating more instances is not the
  target workaround.

### 4.4 `shared` is a type/validation annotation, not a proof

`shared` on a table/global/memory/GC type (and the SET `sharecomptype` /
shared-heap-type encodings) says what may be referenced and which atomic forms
are legal. It does **not** mean any of the following:

- the module is safe to call from two host threads;
- every access to the shared object is atomic (the module author still chooses
  the access form);
- the engine has a multi-mutator GC, waitqueues, or thread lifecycle;
- a `shared` function is reentrant, or uses no host-global state.

It is one input to a G-C/G-D safety argument, not the conclusion.

### 4.5 Initialization and waits

- Active data/element initialisation and the start function happen during
  instantiation, before the instance is published to the embedder. The engine
  orders shared-memory data initialisation with a barrier; the embedder must not
  publish or call the instance concurrently before `instantiate` returns.
- `memory.grow`/`memory.size` are ordered for shared memory (`THREADS.md`
  TH-3); an unshared memory has no cross-agent grow contract, so do not grow it
  while another thread/instance is using it.
- Current `wait`/`notify` are the **single-agent forms**: `notify` returns 0,
  a matching `wait` returns timeout (2), and nothing wakes it. That is a
  documented current-build deviation, not a guest thread-safety primitive.
  A multi-agent build must replace it with a real waiter table before a module
  can rely on wake-up (`THREADS.md` TH-2/TD9/TH-5).


### 4.6 Guest locks and semaphores — an open design, not a current primitive

There is **no standard WebAssembly mutex, `std::mutex` equivalent, or
semaphore object** in this tree. A counting semaphore can be sketched with an
atomic counter plus `wait`/`notify`, but the wake-up and fairness questions
below are the same; nothing here defines one. The current guest tools are
shared-memory atomics and the single-agent `wait`/`notify` forms; a module can
build a lock from an `i32`/`i64` control word using atomic RMW/cmpxchg and, when
the runtime supports it, blocking `wait`/`notify`. That is a **module-level
protocol**, not an engine-provided object, and it has not been standardized
here.

A future guest-lock design has to answer at least:

- **Representation and ABI.** Is the lock a shared-memory word, a shared GC
  object, or an engine-owned opaque object? Who initializes it, who destroys it,
  and what is the owner/recursion representation?
- **Blocking.** Spin, yield/`pause`, `memory.atomic.wait`, or a managed
  waitqueue? How are spurious wakeups, timeouts, interruption and fairness
  defined? The current single-agent `wait` cannot wake, so it is not sufficient
  for a blocking mutex.
- **Reentrancy and ordering.** Non-recursive, recursive or explicit owner? What
  lock-order discipline is required to prevent deadlocks between several guest
  locks and host locks?
- **Traps.** Wasm traps do not run destructors and are not generally catchable
  by Wasm exception handlers. If a thread traps out of a critical section, the
  lock remains held unless the module has a way to run cleanup before the trap
  is observed, or the engine records and releases/poisons it. There is no
  automatic `finally` in Wasm, so a pure module-level lock has no cleanup path
  for an escaping trap; this needs an engine/runtime policy (see §10.1).
- **Tail calls.** A `return_call` cannot release a lock in the caller after the
  call. The toolchain/author must not tail-call out of a critical section, or
  the lock protocol must make that impossible/visible.
- **Host interop.** If a native import waits on a guest lock, or a guest waits
  on a host mutex, both sides need a common protocol. A C++ `std::mutex` and a
  Wasm atomic word are not interchangeable.

Until such a standard exists, the safe statement is: **guest code may implement
its own lock protocol over the available shared-memory/GC atomics, but the
engine does not provide or validate one.** `shared` annotations are necessary
for some of these objects but remain a type restriction, not a lock.


## 5. Host-side / embedder-side thread safety

Host-side safety is about the C++ objects and native callbacks an embedder
touches. It is **not** a statement that generated Wasm code is race-free.

### 5.1 The engine object model

| Object | Mutability / ownership | Current concurrency rule |
|---|---|---|
| `WASM::Store` (process-wide singleton) | Owns memories, tables, tags, call tags, GC heap, strings; IDs/pointers stable | Management containers are mutex-guarded; `TypeRegistry` serializes interning; budget accounting is atomic. This protects **Store metadata**, not guest data. |
| `WASM::Module` | Decoded, immutable after parse; the target runtime owns **one** instance for it | Treat one `Module` as read-only after parsing. Current test/spec code may instantiate it more than once; that is not a concurrency model. |
| `WASM::ModuleInstance` | Mutable: globals, memory/table references, GC region, trap-visible state | **Not internally synchronized.** One call at a time; guard it with an external mutex today. The target is instance-level synchronization, not one instance per thread. |
| `ModuleInstantiator` / backend session | Mutable compiler state and code cache | **Not guaranteed reentrant.** Use one instantiator per host thread or serialize calls. Backend-specific build locks are not a general contract. |
| `WASM::Callable` | Immutable function pointer, context, type identity after publication | May be read from multiple threads; the code and context must outlive every call. Do not mutate a `Callable` while calls are in flight. |
| `WASM::VMContext` | Per-instance POD passed as arg0; contains index-space pointers, globals, `hostData`, `hostBridge` | Read by generated code without a lock. It must not be destroyed or re-pointed while a call using it is active. |
| `hostData` | Embedder-owned pointer inside `VMContext` | **Entirely host-owned.** The engine never synchronizes it; the embedder must. |
| `WASM::HostBridge` | Borrowed embedder-owned callback table (`retain`/`release`/`equals`, `instanceCreated`/`instanceDestroying`) | **Not synchronized by the engine.** Callbacks run on whichever host thread executes the instance; the adapter must serialize its own state. A bridge must outlive the instance and its callbacks must not block on a Wasm call. |
| `WASM::TrapScope` | Per-host-thread longjmp target | Must be constructed on the thread that will execute the call. Never share a `TrapScope` across threads. |
| `WASM::g_tailCallState` | `thread_local` pending tail-call state | Generated code asks `WASM::currentTailCallState()` for the executing host thread's address; a host thread never uses another thread's state. |
| `GcHeap` / `StringStore` | Store-owned host metadata plus allocator locks | Their host-side bookkeeping is locked; generated Wasm code still has a single mutator and is not a multi-agent GC. |

### 5.2 What the current engine does and does not guarantee

**It does guarantee (within the current single-agent model):**

- per-host-thread trap and tail-call state;
- Store/type/GC/string **management** locks, so concurrent creation/lookup of
  Store entities and one-instance setup do not corrupt the Store's containers;
- stable Store-owned entity addresses for the lifetime of the Store;
- atomic, naturally aligned atomic accesses and alignment traps;
- ordered shared `memory.grow`/`memory.size` and the data-segment barrier;
- a host-thread-local `TrapScope` and a per-thread tail-call state.

**It does not guarantee:**

- that two host threads may call the **same** `ModuleInstance` (or the same
  exported function on it) concurrently;
- that one `ModuleInstantiator`/backend session is safe for concurrent
  compilation;
- that a `Module` or `ModuleInstance` may be mutated while another thread reads
  it;
- that creating several instances of a module is a supported isolation or
  thread-safety mechanism — the target is one instance per module;
- that arbitrary concurrent module parsing is a verified immutable-snapshot
  operation for code that reads canonical type metadata (interning is
  serialized; the read path is not a formally snapshotted type store);
- multi-mutator GC, cross-agent reference counting, waitqueues, or thread
  lifecycle;
- any property of a native import or `hostData` implementation;
- that a module using atomics is data-race-free (only the atomic operations
  themselves are tear-free and ordered);
- that `Module`/`ModuleInstance` lifetimes are reference-counted.

The `Store` locks are a **metadata-safety** measure, not a global interpreter
lock. They do not serialize calls, memory accesses or GC mutation.


### 5.3 Native imports (Wasm → native C/C++)

A native import is registered as a `Callable` with a `fnPtr`; the engine passes
a `VMContext*` and the typed arguments. The engine's obligations are only:

- perform the call with the ABI documented in [`ABI.md`](ABI.md);
- pass the caller's context when `Callable::context == nullptr`, or the
  registered context when non-null;
- keep the call path itself free of unsynchronized engine state.

The **embedder** owns everything else:

1. **Thread safety/reentrancy.** If multiple host threads or future Wasm agents
   can reach the import, the function must be reentrant and internally
   synchronized. The engine does not serialize imports.
2. **`hostData`.** The same `hostData` pointer is visible to every call; the
   native function must lock or use atomic host state. A plain `int` counter is
   not safe merely because it lives in C++.
3. **Exception boundary.** Do not let a C++ exception propagate through a JIT
   frame. Catch it in the import and return a Wasm value or call `WASM::trap`.
   A trap uses `longjmp`, so destructors between the trap point and the
   `TrapScope` do not run; keep the native path POD-only or exit normally
   (see [`SAFETY.md`](SAFETY.md) §2.6).
4. **Lifetime.** The `Callable`, its `context`, and any `VMContext` it captures
   must outlive every invocation. Cross-module contexts are burned into code at
   compile time ([`ABI.md`](ABI.md)), so destroying an instance while another
   instance can call it is a use-after-free even with no threads.
5. **Shared data.** If the import reads/writes linear memory or GC state that
   Wasm agents share, it must use the same atomic/ordering discipline as the
   module. A host `std::mutex` does not make a non-atomic Wasm access safe
   against an unrelated Wasm agent; both sides must agree on the protocol.
6. **Trap context.** A native import runs while the calling thread's
   `TrapScope` is armed. It must not assume it can longjmp to a different
   thread's scope, and must not leave host locks held on a path that calls
   `WASM::trap`.

### 5.4 Exported Wasm functions (host → Wasm)

An export is just a `Callable`; `callCallable`/`callCallableWithResults` invoke
its raw entry point. There is no hidden per-instance lock. Therefore:

- **Same instance:** the only target model is the module's single instance. The
  embedder must externally serialize calls today; a future engine may provide
  per-instance synchronization. Two host threads calling the same export
  concurrently race on globals, memory and GC state even if the function is
  “pure” in its own view.
- **Separate instances:** current API/tests may create them, but they are not
  the intended concurrency architecture. Do not use instance multiplication as
  a substitute for making the one instance's state safe.
- **Exported function contract:** the Wasm type system has no `threadsafe`
  attribute. A module author must document which exports are reentrant,
  serial-only, and what shared state they touch. The engine cannot check this.
- **Destruction:** no call may be in flight when its instance, module or
  `Callable` is destroyed.
- **Trap state:** each host thread that catches traps needs its own
  `TrapScope`; `callCallable` does not create one implicitly.

### 5.5 Host code that shares data with Wasm

If the host and a Wasm agent share a buffer, the host side is another mutator.
The host must use an atomic/locking protocol compatible with the Wasm side:

- use `std::atomic`/`__atomic` for the same naturally aligned locations, or an
  external mutex that all participants honour;
- never perform a plain C++ load/store racing with a Wasm atomic or another
  plain access;
- treat `memoryBase`/`memorySize` as engine-managed: do not cache an unshared
  memory's `memoryBase` across a grow, and do not grow it while a call is using
  it;
- for shared memory, `memoryBase` is stable (the engine reserves the maximum),
  but concurrent data accesses still need the module's atomic protocol.


### 5.6 C++ mutexes, RAII, traps and tail calls

For **host-only** state, yes: `std::mutex` and `std::lock_guard` are supported
and are the expected primitive for protecting `hostData` and other native state.
A native import may use `std::mutex` and `std::lock_guard` for **host-side**
mutual exclusion; the engine does not ban them and nothing in the import ABI
requires lock-free code. **Verdict:** they are the right host primitive, but
only with the no-trap/no-cross-boundary discipline below; they are not safe as
an RAII fence around an arbitrary Wasm call. The current trap path does not
unwind C++:

- `WASM::trap` uses `longjmp`. Destructors between the trap point and the armed
  `TrapScope` do **not** run. A `std::lock_guard` whose scope contains a
  potentially trapping Wasm call can therefore leave its `std::mutex` locked.
- This covers a native import that calls `WASM::trap` itself, and a native
  import that calls a Wasm export which later traps or calls back into native
  code that traps.
- If the embedder holds a mutex while calling a Wasm export, and a Wasm import
  tries to take the same mutex, a non-recursive mutex deadlocks. A recursive
  mutex only postpones the problem; it does not define reentrancy semantics.
- **Tail calls** do not themselves `longjmp` in the current implementation: the
  per-function driver returns normally through the trampoline. A trap in the
  tail-called chain still has the same `longjmp` behaviour, and a guest lock or
  host lock held across the tail call has no automatic release point.

Practical rules under the current engine:

1. Use `std::mutex`/`lock_guard` only around native-only critical sections that
   cannot call back into Wasm and cannot trap. This is safe and supported.
2. If a native import must call back into Wasm, use an explicit unlock before
   the call, or a lock protocol that does not use RAII across the boundary; do
   not rely on the destructor running after a trap.
3. Do not hold a host mutex across an export call unless every import reachable
   from it is proven not to take that mutex (and the trap leak is accepted or
   prevented).
4. Never hold a guest lock across a `return_call`; there is no caller frame left
   to release it. This is a toolchain/module-author rule until the engine
   defines automatic release/poisoning.
5. Treat the trap/RAII interaction as an **open design item**, not a solved
   guarantee. A future engine may replace `longjmp` with an unwind-safe trap
   mechanism, add explicit lock ownership tracking, or require a host cleanup
   protocol; until then, RAII is safe only where no trap can cross it. The
   candidate cleanup protocol — and why it still cannot save a non-trivial
   automatic object in a skipped frame — is in
   [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md).

The same caveat applies to `std::unique_lock::unlock()` calls: they are safe
only if control returns; `longjmp` bypasses the remainder of the native frame.

## 6. Intersections and non-implications

The two domains overlap at the export/import boundaries and on any object that
is shared by both sides. These are the cases where a claim about one domain is
most often mistaken for a claim about the other.

| # | Statement | Why it is not enough |
|---|---|---|
| I1 | “The engine locks the Store, so Wasm is thread-safe.” | Store locks protect maps/IDs/allocator metadata. They do not protect linear memory, globals, tables, GC object fields, or a call's operand stack. |
| I2 | “The module uses atomics, so its exports are host-threadsafe.” | The export may still touch unshared globals/GC state, call a non-reentrant native import, or be entered twice on the same instance. Atomics only protect the locations they name. |
| I3 | “The native import is thread-safe, so the Wasm side is thread-safe.” | The import may read/write shared Wasm memory non-atomically, call back into the same instance, or use `hostData` unsafely. |
| I4 | “The module is marked `shared`, so it is safe to call concurrently.” | `shared` is a type/validation restriction; it does not prove data-race freedom, and the current runtime has no multi-agent scheduler or shared GC heap. |
| I5 | “The test suite passes with atomics, so concurrent agents are tested.” | The current suite executes the single-agent forms. There is no second Wasm agent to expose a missing acquire/release edge, a waiter bug, or a GC/refcount race. |
| I6 | “Two calls are independent because they use different host threads.” | They use the same single `ModuleInstance`, so they share its mutable state. Instance multiplicity is not the target isolation mechanism; serialization or instance-level safety is required. |
| I7 | “Serialization by a host mutex makes non-atomic Wasm accesses safe.” | Only if **every** path into the data (host and every Wasm agent) honours the same protocol. A host mutex around one entry point does not stop another agent entering a different function. |
| I8 | “The instance is alive because the host holds a `Module`.” | `Module` and `ModuleInstance` have independent lifetimes, and compiled cross-module calls bake `VMContext*` addresses into code. Lifetime is a separate precondition, not a thread-safety property. |
| I9 | “The instance is published after `instantiate`, so its start/import call can be concurrent.” | Initialization, data segments and start run **before** publication. Calling while `instantiate`/start is still running is a race. |
| I10 | “A trap is just an error return, so host locks survive it.” | `WASM::trap` uses `longjmp`; destructors between the trap and the armed `TrapScope` do not run. A lock held across a trapping native import can remain locked. |
| I11 | “The native callback may throw C++ exceptions; the engine will handle them.” | There is no C++ exception unwind contract across a JIT frame. Catch inside the callback and return a value/trap. |
| I12 | “An import called from Wasm is single-threaded because Wasm has no threads.” | Wasm agents are deferred, but the **host** may call the one instance from several threads; the same `Callable`/`hostData` can therefore be reached concurrently without any Wasm agent. |
| I13 | “Creating a new instance per thread is a valid workaround.” | The intended runtime is one instance per module, one `VMContext` per instance. Multiple instances are current test/API incidental, not a supported thread-safety architecture. |

Two practical consequences:

- **Host locks must not be held across an arbitrary Wasm call** unless the
  embedder has proven the module cannot re-enter the same lock through an
  import, and unless the trap path cannot bypass its release. Prefer
  per-instance ownership or a reentrant protocol.
- **Guest synchronization must be agreed with the host**, not imposed
  unilaterally by either side. A host `std::mutex` and a Wasm `atomic` are not
  interchangeable unless both sides use the same memory locations and protocol.

## 7. Responsibility matrix

`E` = engine/backend author (this library); `M` = Wasm module/toolchain author;
`H` = embedder. Each non-empty cell is an obligation of that party; `—` means
the party has no obligation for that row.

| Scenario | E | M | H |
|---|---|---|---|
| G-A: one agent, serial calls on the single instance | No hidden concurrency; preserve one `ModuleInstance`/`VMContext` | Do not assume concurrent entry; document serial-only exports | Serialize calls to the instance |
| G-B: single instance, host-serialized entry | No implicit instance safety today; target is instance-level synchronization | State the export/import contract for re-entry | Guard the single instance with an external mutex; do not create per-thread instances |
| G-C: shared memory in the single instance | Provide ordered atomics/alignment traps and size/grow ordering | Use atomic accesses/orderings for every shared location; no racing plain access | Use compatible atomics/locks for host accesses |
| G-D: shared table/global/GC in the single instance | Provide annotations/atomics; multi-mutator GC is **future** | State G-D only if the runtime supports it; use SET rules | Do not claim G-D until engine/agents exist |
| H→W export | No implicit instance lock; preserve ABI/lifetime; target per-instance protocol | Declare each export's reentrancy/state contract | Serialize the single instance or wait for engine instance safety |
| W→H import | Call the registered `fnPtr` with the correct context | Call only imports whose contract permits it | Make import reentrant/thread-safe; synchronize `hostData`; no C++ exceptions across the boundary |
| Store/API management | Locks, stable IDs, per-thread trap/tail state | — | Treat `Module`/`ModuleInstance`/instantiator as not concurrently mutable |
| Initialization | Apply data/elem/start before publication; barrier | Do not rely on concurrent initialization | Do not publish/call until `instantiate` returns |
| Trap/failure | `TrapScope` per thread, POD trap path | Use Wasm traps, not host crashes | Arm a `TrapScope` per thread; expect destructors to be skipped on trap |
| Lifetime | Preserve code/context addresses | Keep exports/imports valid | Keep modules/instances/callables alive through every call; no destruction in flight |

The library author (this project) must **not** infer guest safety from any of
its own host-side mechanisms. In particular, it must not:

- describe `Store`/type/GC/string locks as a global interpreter lock or as
  protection for linear memory, globals, tables or GC fields;
- advertise single-agent atomic tests as multi-agent race-freedom or
  acquire/release-edge tests;
- claim `shared` annotations are a proof of data-race freedom;
- present multiple instances of a module as the concurrency/isolation plan;
- claim `std::lock_guard` survives a trap or that its destructor runs on the
  `longjmp` path;
- add a hidden per-instance or per-import global lock without making it part of
  the documented contract (and without considering the trap/longjmp path);
- promise that a native import, `hostData`, `Callable` or `ModuleInstance` is
  safe to share unless the corresponding section above says so.



## 8. Current implementation status

This is a status statement, not a conformance claim. “Supported” means the
engine has the mechanism and the documented host protocol can use it; “not
supported” means the operation is outside the current contract and its result
is undefined even if a particular run happens to work.

| Property | Current status | Where |
|---|---|---|
| **One `ModuleInstance` per `Module`, one `VMContext` per instance** | **Target architecture**; no per-thread instances | `ModuleInstance`, `VMContext` |
| One agent/call at a time on that instance | **Supported** (the only execution model) | `ModuleInstance`, `callCallable` |
| Several host threads entering the **same** instance | **Not supported** | no instance lock |
| Several host threads with **separate** instances | Current API/test incidental only; **not a target concurrency model** | `Test/main.cpp` host-thread test |
| Concurrent `ModuleInstantiator`/backend compilation | **Not guaranteed**; use one per thread or serialize | backend session |
| Concurrent `Module`/`ModuleInstance` mutation | **Not supported** | mutable fields |
| Store entity creation/lookup and type interning | **Management-level locks**; not a guest-data lock | `WasmStore`, `TypeRegistry` |
| Host-thread trap state | **Per host thread** | `TrapScope` / `trapState()` |
| Trap-time host cleanup (locks, roots, arenas) | **Absent**; `longjmp` skips destructors, so release before a trap-capable call or use a protocol that does not rely on RAII | §5.6, [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) |
| Host-thread tail-call state | **Per host thread**; generated code resolves it dynamically | `currentTailCallState()` |
| Shared-memory atomic loads/stores/RMW/cmpxchg | **Implemented single-agent**, hardware atomics and alignment traps | `wasm_atomic.*`, `THREADS.md` TH-1 |
| Acquire-release ordering and `pause` | **Implemented single-agent** | `ACQUIRE_RELEASE_ATOMICS.md` |
| Shared `memory.grow`/`memory.size` and data-init barrier | **Implemented** | `THREADS.md` TH-3 |
| Shared table/global/segment annotations | **Implemented/partial**; full shared-to-shared restriction checking remains | `SHARED_EVERYTHING_THREADS.md` SET-1 |
| `global/table/struct/array.atomic.*` (working draft) | **Implemented single-agent**; reference-slot RC/shared GC is not complete | SET-3/SET-2 |
| `wait`/`notify` | **Single-agent forms only**; no wake-up, timeout not slept | `THREADS.md` TH-2/TD9 |
| Guest mutex/semaphore standard | **Absent**; only module-authored atomics over current primitives | §4.6 |
| Multi-agent scheduler, shared GC heap, TLS, waitqueue, lifecycle | **Absent/deferred indefinitely** | `THREADS.md` TH-5; SET-4/5 |

The host-thread test exists to exercise the **current** API's independent
session/instance setup; it deliberately does **not** test the target
single-instance concurrency model. Passing it is not evidence that concurrent
calls into the one instance are safe.

## 9. Minimum embedder protocol

The smallest protocol that is valid with the current engine is:

1. **Bracket every call with a `TrapScope` on the calling thread.** Do not share
   the scope; do not assume C++ destructors run between `WASM::trap` and the
   scope.
2. **Treat the module's single instance as the unit of serialization.** Guard
   it with an external mutex (or permit only one host thread) today. Do not
   create per-thread instances as a design; that is not the target model. The
   long-term requirement is an engine/instance-level concurrency protocol.
   Do not hold the external mutex across an import that can call back into the
   same instance unless it is recursive or the callback is proven non-reentrant.
3. **Use one `ModuleInstantiator`/backend session per thread**, or serialize
   compilation. Do not compile into the same context from two threads.
4. **Keep modules, instances, callables, contexts and result buffers alive**
   until every call using them has completed. Cross-module `VMContext*`
   addresses are baked into code.
5. **Make each native import thread-safe if it can be reached concurrently**;
   synchronize `hostData`; catch C++ exceptions inside the import; return a
   Wasm trap instead of throwing across the JIT frame. Use `std::mutex`/
   `lock_guard` for host-only state only, and see §5.6 before crossing a
   trapping/tail-calling Wasm call with a lock held.
6. **State the guest model for each module** (G-A/G-B/G-C/G-D). Use G-C/G-D
   only when the whole data-sharing protocol — Wasm and host — uses compatible
   atomics/locks. Do not infer it from `shared` alone.
7. **Do not grow an unshared memory while it is in use.** For shared memory,
   `memoryBase` is stable and size/grow are ordered, but guest and host data
   accesses still need the agreed atomic protocol.
8. **Do not publish an instance from another thread before `instantiate`
   returns**, and do not destroy objects while calls are in flight.

If an embedder needs concurrent calls into the single instance, the current
engine cannot provide that contract. The correct options are: add host-side
serialization, or wait for the engine's per-instance concurrency/agent
mechanism. Creating more instances is not the intended workaround.


## 10. Future multi-agent contract (non-normative)

True multi-agent execution is **deferred indefinitely but not ruled out**
(`THREADS.md` TH-5). The following is the shape any future implementation must
preserve; it is not a current guarantee:

- The target remains **one `ModuleInstance` per `Module`** and one `VMContext`
  per instance. Agents must not be modelled as per-thread copies of the
  instance; per-agent stack/trap/tail state lives outside the instance state.
- An instance-level concurrency protocol (engine lock, per-agent executor, or a
  documented guest/host protocol) must be designed; “make another instance” is
  not an escape hatch. Candidate directions are a per-instance exclusive lock
  (simple, no guest parallelism), an engine-managed agent scheduler with
  per-agent stacks over the one instance, or module/toolchain-managed guest
  locks over shared state; none is chosen or implemented here.
- Each agent has its own evaluation stack, trap target and tail-call state;
  these must not live in process-global mutable variables. The current
  host-thread-local state is the first step.
- A `VMContext` remains the native ABI's arg0, but an agent's current context
  must not be recovered from a process-global “current instance” slot.
- The Store remains the shared ownership boundary. Shared memory/table/GC
  objects need a multi-mutator protocol; unshared memories remain single-owner.
- The GC must either gain a shared-heap/multi-mutator collector or reject a
  shared-GC module; the current RC heap and reference-slot atomics are not a
  cross-agent collector.
- `wait`/`notify` need a real waiter table; the current single-agent return
  values must not be treated as a multi-agent synchronization primitive.
- Native imports must be documented as potentially concurrent; the embedder
  must make them reentrant. The engine may not add a global import lock without
  documenting it as part of the contract.
- An export API for agents needs an explicit concurrency mode; “a wasm function
  pointer” is not a thread-safe handle by itself.
- Guest locks/semaphores (§4.6), host `std::mutex`/RAII (§5.6), and the trap
  path (§10.1) are **open design items**, not established semantics.

### 10.1 Traps, lock ownership, cancellation, and tail calls

The current trap mechanism is a per-host-thread `longjmp` to an armed
`TrapScope`. That model does not yet answer the multi-agent questions:

- **Per-agent trap isolation.** A trap in one agent must unwind only that
  agent's stack and must not disturb another agent's trap target, operand stack
  or tail-call state. Today `TrapScope`/`trapState()` are per host thread;
  agent-aware execution needs per-agent state even if several agents share a
  host thread.
- **C++ unwinding.** `longjmp` skips destructors inside native imports and
  host call frames. `std::lock_guard` therefore cannot be relied on across a
  potentially trapping Wasm/native call (§5.6). A future design must choose
  between a genuinely unwind-safe trap (for example, exceptions with a
  documented ABI and unwind tables), explicit cleanup before every trap-capable
  call, or an engine-owned lock registry that can release/poison locks on trap.
  The explicit-cleanup branch has a candidate shape: a fixed-capacity,
  Go-style LIFO cleanup stack in the per-thread trap state, with `trap()`
  draining `noexcept` thunks before `longjmp`. It gives native imports a way to
  unlock or release external state, but it is not C++ unwinding and does not
  make non-trivial locals in skipped frames safe. See
  [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) for the design, the code sketches, the
  comparison table, and the current trapping-start-function gap.
- **Guest lock ownership.** A Wasm trap is not a C++ exception and does not run
  a `finally`. If an agent traps while holding a guest mutex, the lock stays
  held unless the module can prevent/convert the escaping trap into a cleanup
  path, or the engine records and releases/poisons it. There is no current
  ownership tracking.
- **Host lock ownership.** A host mutex held by a native import that calls
  `WASM::trap`, or by a host frame around a trapping export, can remain locked
  after the longjmp. A lock left held by a dead/trapped agent is a deadlock for
  the surviving agents.
- **Tail calls.** The trampoline returns normally, but a trap in the
  tail-called chain still jumps out without C++ cleanup. A guest lock must not
  be held across `return_call`; a host lock must not be held across a call that
  can tail-call into a trap. Penalizing or forbidding tail calls inside critical
  sections is a toolchain/module-author rule until the engine defines
  otherwise.
- **Cancellation/agent teardown.** Killing an agent (rather than letting a trap
  propagate) needs an explicit policy for locks, waiters, GC references and
  partially executed critical sections. “Trap equals return” is not enough.
- **Instance state after a trap.** Is the single instance reusable after a trap
  in one agent, or must it be poisoned/reinitialized? A trap that leaves guest
  data, locks or a tail-call chain half-updated needs an explicit state machine;
  the current engine has no such policy for a concurrent instance.

This is an intentionally open design section. The engine must not silently
promise unwind/cleanup semantics it does not implement. Any future agent
implementation should define trap ownership, cleanup and lock-poisoning before
advertising G-C/G-D safety.


Until those points are implemented, the only valid guest model for shared
state remains the documented single-agent forms. The tests that cover atomics
and shared annotations prove decoding/lowering and single-agent execution, not
multi-agent race freedom.

## 11. References

- In-repo:
  [`SAFETY.md`](SAFETY.md) (trap/longjmp, locks, allocation),
  [`ABI.md`](ABI.md) (`VMContext`, `Callable`, native imports),
  [`proposals/THREADS.md`](proposals/THREADS.md) (execution model, TH-3/TH-5),
  [`proposals/SHARED_EVERYTHING_THREADS.md`](proposals/SHARED_EVERYTHING_THREADS.md)
  (SET-1…SET-3, deferred shared GC/TLS/waitqueues),
  [`proposals/ACQUIRE_RELEASE_ATOMICS.md`](proposals/ACQUIRE_RELEASE_ATOMICS.md)
  (orderings and `pause`),
  [`proposals/GC.md`](proposals/GC.md) (reference counting and the single
  mutator), [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) (trampoline and
  per-function drivers), [`TRAP_CLEANUP.md`](TRAP_CLEANUP.md) (the Go-style
  trap-cleanup candidate and the start-function gap),
  [`TESTING.md`](TESTING.md) (the host-thread test and how to run it).
- Sources:
  `WasmBase/WasmStore.*` (Store locks and stable entities),
  `WasmBase/WasmTypeRegistry.*` (intern serialization),
  `WasmBase/WasmGcHeap.*` / `WasmBase/WasmString.*` (host-side recursive locks),
  `WasmBase/WasmValue.hpp` (`Callable`, per-thread tail state),
  `WasmBase/WasmTrap.*` (thread-local trap state),
  `WasmBase/wasm_atomic.*` (ordered atomic helpers),
  `WasmBase/WasmMemory.*` / `WasmStore.cpp` (shared-memory grow/size ordering),
  `WasmBase/WasmModuleInstance.cpp` (instance state and initialization).
- Specifications:
  [WebAssembly threads](https://github.com/WebAssembly/threads),
  [shared-everything threads](https://github.com/WebAssembly/shared-everything-threads),
  [acquire-release atomics](https://github.com/WebAssembly/acquire-release-atomics),
  and the C++ memory model as implemented by the host compiler's `__atomic`
  builtins.

