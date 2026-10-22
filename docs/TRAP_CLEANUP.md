# Trap Cleanup — Escaping the POD-Only Trap Path with a Go-Style Defer Stack

Status: design note / open design item (revision 1 — records a candidate
mechanism and a concrete current gap; **no runtime change is implemented**).
This is not a WebAssembly proposal note and does not change the current
`TrapScope` contract.

Audience: anyone touching `WasmBase/WasmTrap.*`,
`WasmBase/WasmModuleInstance.*`, backend trap emission, native imports, or the
optional [`HostBridge`](ABI.md#external-host-adapter-optional-hostbridge) host
adapter.

The current trap contract is deliberately small: a trap is a `longjmp` to an
embedder-armed `WasmBase/WasmTrap.hpp` `TrapScope`; the generated frames carry
no unwind tables, so a C++ exception cannot cross them. The consequence is the
POD-only rule in [`SAFETY.md`](SAFETY.md) §2.6: native helpers on a trap-capable
path must hold only trivially destructible locals, because a `longjmp` skips
destructors between the trap point and the scope.

**So here is how we could escape the POD-only constraint caused by traps:** not
by pretending `longjmp` runs C++ destructors, but by giving the trap path an
explicit, fixed-capacity, Go-style cleanup stack that `WASM::trap()` drains
before it jumps. That is an opt-in cleanup protocol, not general C++ unwinding.
It can release a host lock, a per-call handle arena or a temporary native
resource; it cannot make `std::unique_ptr`, `std::string` or `std::lock_guard`
destructors run in a frame that is being skipped.

## 1. Summary

- `WASM::trap()` is a non-local exit, not a C++ throw. C++ RAII works on normal
  return and C++ exceptions, but not across `longjmp`.
- The closest useful analogue to Go's `defer` is a LIFO cleanup stack attached
  to the per-thread trap state. `TrapScope` is the panic target; the stack is
  what runs on the way out.
- The cleanup records must be preallocated and non-throwing. Normal returns must
  dismiss their records explicitly; there is no C++ compiler support that can
  make a `defer` statement automatic in a trap-skipped frame.
- A defer stack **does not relax** the POD-only rule. It gives native code a way
  to release *external* state (mutexes, arenas, handles) before the jump; it does
  not make non-trivial automatic objects safe in skipped frames.
- The bookkeeping belongs in thread-local state, not in a normal `TrapScope`
  member whose fields change after `setjmp()`. C leaves changed non-`volatile`
  automatics local to the `setjmp()` frame indeterminate after `longjmp`.
- There is a concrete current gap: a trapping start function can jump out of
  `ModuleInstantiator::instantiate` while it holds a
  `std::unique_ptr<ModuleInstance>`, so the instance destructor and the
  `HostBridge::instanceDestroying` callback do not run. That path needs a normal
  C++ catch boundary (or a trivially destructible holder) in addition to any
  defer stack.
- Recommended order: fix the start-function cleanup path first, then add the
  generic defer stack only if native imports need general trap-time cleanup.
  Do not replace `longjmp` with C++ exceptions merely for this.

## 2. The trap model we have

`WasmBase/WasmTrap.cpp` currently does three things in `trap()`:

1. records `TrapKind` and the message in thread-local `TrapState`;
2. resets the tail-call trampoline's pending target;
3. `longjmp`s to `TrapState::active`, or prints and aborts if no `TrapScope` is
   armed.

`WasmBase/WasmTrap.hpp` documents the intended embedding pattern:

```cpp
WASM::TrapScope scope;
if (setjmp(scope.buffer) == 0) {
    result = callIntoWasm();
} else {
    std::fprintf(stderr, "trap: %s\n", scope.message());
}
```

That is the whole cleanup machinery today. The generated code and the native
helpers it calls are required to be POD-only on the trap-capable path
([`SAFETY.md`](SAFETY.md) §2.6). This is not a stylistic preference: if a frame
between the trap point and the `TrapScope` contains an automatic object with a
non-trivial destructor, `longjmp` leaves its destructor unrun and the behavior
is not defined by C++.

## 3. Why RAII stops at a trap

RAII is the right mechanism for the rest of the engine. Loader, instantiator and
compiler failures are C++ exceptions; `JitBuildScope` makes a begin/end lock
exception-safe; the allocation-budget code uses normal C++ ownership. None of
that gives a trap-safe cleanup point.

| Exit path | C++ RAII | Go-style `defer` | Current `WasmJit` |
|---|---|---|---|
| Normal return | destructors run | deferred calls run | normal C++ destructors run |
| C++ exception | destructors run while unwinding | only if explicitly integrated | loader/instantiator/JIT paths use exceptions and RAII |
| Wasm `longjmp` trap | **destructors skipped** | can run if `trap()` executes the stack first | no cleanup hook; native path must be POD-only |

The last row is the gap this note addresses. A C++ scope guard, including
`std::scope_exit`, is still a destructor-based mechanism and is no better than
`std::lock_guard` here.

## 4. Go's `defer` as the design inspiration

Go's `defer` is not magic; it is runtime-supported cleanup. The runtime keeps a
per-goroutine LIFO list of deferred calls. On a normal return it runs the
function's defers; on `panic` it runs them while unwinding, and `recover` can
stop the panic at a frame that has one. The important properties are:

- the cleanup action is explicit and scoped to a function;
- execution order is LIFO;
- the runtime, not each caller, drives it on both normal and panic exits;
- deferred arguments are evaluated at registration time.

C++ has no equivalent runtime instrumentation for arbitrary frames, and this
engine's generated frames have no unwind tables at all. We cannot make a literal
`defer` keyword work. We can copy the useful part: an explicit LIFO list of
cleanup thunks owned by the trap state.

| Go concept | Candidate `WasmJit` equivalent | Difference |
|---|---|---|
| per-goroutine defer list | per-host-thread cleanup stack in the trap state | future multi-agent work needs per-agent state, not just per-thread |
| `defer f(x)` | `scope.defer(fn, arg)` / a `TrapDefer` token | no compiler syntax; the caller must dismiss on the normal path |
| `panic` | `WASM::trap()` | trap is `longjmp`, not a typed Go panic |
| `recover` | `TrapScope` seeing `setjmp() != 0` | no resumption; the embedder receives the trap kind/message |
| defers run on normal return | explicit `dismiss(token)` before return | requires discipline; a stale record would run on a later trap |
| defers run during panic unwinding | `trap()` runs records down to the active scope's snapshot | runs before `longjmp`, while the native frames are still physically active |

The last row is also the safety boundary: the thunks run **before** the jump,
so they may use pointers to state that is still alive in the native frame.
After the jump, however, every skipped frame is gone and a deferred destructor
would not have been a substitute for C++ lifetime rules.

## 5. Candidate design: a trap-cleanup stack

The identifiers below are proposals, not APIs that exist today. The current
`TrapScope` only has `buffer`, `message()` and `kind()`.

### 5.1 API sketch

```cpp
WASM::TrapScope scope;
if (setjmp(scope.buffer) == 0) {
    hostMutex.lock();

    // Register after acquiring the resource. `defer` must not allocate or throw.
    auto token = scope.defer(unlockThunk, &hostMutex);

    callIntoWasmThatMayTrap();

    scope.dismiss(token);   // normal path: remove before the resource is gone
    hostMutex.unlock();
} else {
    // `trap()` already ran unlockThunk before longjmp.
}
```

A native import needs a way to reach the active scope. A small accessor is
better than growing `VMContext` for this:

```cpp
WASM::TrapScope* trap = WASM::currentTrapScope();
if (trap != nullptr) {
    hostMutex.lock();
    auto token = trap->defer(unlockThunk, &hostMutex);
    // ... call back into Wasm, which may trap ...
    trap->dismiss(token);
    hostMutex.unlock();
}
```

If no scope is armed, `WASM::trap` aborts anyway; an import that wants cleanup
must either require a scope or perform its cleanup before calling `trap`.

The stack should be a fixed-capacity array of trivially copyable records:

```cpp
struct TrapCleanup {
    void (*fn)(void*) noexcept;
    void* arg;
};
```

No `std::function`, `std::vector`, `std::string` or allocating closure belongs
on this path. A fixed cap plus an explicit failure result (not a trap) keeps the
trap path allocation-free.

### 5.2 Trap path

When `WASM::trap(kind, detail)` is called:

1. set `TrapState::kind`/`message`;
2. reset the tail-call trampoline state (already done today);
3. mark cleanup dispatch as in progress so a cleanup thunk cannot recursively
   trap or register another cleanup;
4. invoke the active scope's records in reverse registration order, down to the
   scope's saved stack depth;
5. `longjmp` to the active scope, or print and abort if none is armed.

The saved depth is what makes nesting work. A `TrapScope` records the
thread-local cleanup top at construction; after a normal return its destructor
verifies that the top is back to that depth (a stale registration is a bug), and
after a trap the stack has already been drained to that depth.

Tail calls are not a separate escape: the trampoline returns normally, but a
trap in the tail-called chain still drains the active scope before jumping. A
lock held across `return_call` has no automatic release point beyond this
cleanup protocol.

### 5.3 Normal path and ownership of a record

Go's compiler makes normal-return cleanup automatic. C++ does not, so the
candidate API has an explicit token:

- `defer(fn, arg)` pushes a record and returns a value token (an index or a
  generation-stamped handle);
- `dismiss(token)` removes the record and must be called exactly once on the
  normal path, in LIFO order relative to other live records;
- a double dismiss, a dismiss of a non-top record, or a missing dismiss is a
  contract violation and should be asserted in debug builds;
- a `TrapDefer` object with a non-trivial destructor would reintroduce exactly
  the problem it is meant to solve if it lived in a frame that can be skipped,
  so the registration token should be trivial or the API should be free
  functions only.

The normal path itself can still use RAII for resources that never cross a
trap-capable call. The explicit protocol is only for the resource that must be
released if the call traps.

### 5.4 Nested scopes, `setjmp`, and thread state

The cleanup stack must live in the thread-local trap state, not as a normal
member of a `TrapScope` local whose contents change after `setjmp()`. C says
that non-`volatile` automatic objects local to the function containing
`setjmp()` and changed between `setjmp()` and `longjmp()` are indeterminate
after the jump. A defer record added after `setjmp()` is such a change.

The safe shape is:

- `TrapScope` saves the previous active scope and the previous cleanup depth in
  fields written **before** `setjmp()`;
- the records and top index live in thread-local storage;
- `trap()` drains to the active scope's saved depth;
- a nested `TrapScope` catches a trap from its own region, drains only its
  records, and restores the outer depth when it is destroyed.

For the current single-agent engine, thread-local is sufficient. The future
multi-agent model in [`THREADSAFETY.md`](THREADSAFETY.md) §10.1 needs the same
state per agent, because several agents may share a host thread.

### 5.5 Cleanup-thunk contract

Every cleanup thunk must be:

- `noexcept`: an exception escaping a cleanup callback during trap handling has
  no safe propagation path through the JIT frames;
- non-trapping and non-reentrant: it must not call `WASM::trap`, enter Wasm, or
  invoke a host callback that can do either;
- allocation-free or allocation-failure-tolerant;
- limited to external state whose lifetime is still valid at the trap point
  (a mutex, an arena, an OS handle, an engine-independent host root);
- ordered so that releasing a lock or closing a handle cannot invalidate an
  earlier record's argument.

If a cleanup thunk cannot satisfy those rules, the resource does not belong on
the trap path. It belongs in an outer frame that catches the trap normally, or
in instance teardown.

## 6. What this can and cannot clean

The most tempting use is also the most dangerous:

```cpp
std::lock_guard guard(hostMutex);   // non-trivial destructor
callIntoWasmThatMayTrap();          // a trap longjmps past guard
```

A deferred `unlockThunk` can make the mutex usable again, but it does **not**
make the `lock_guard` destructor run, and it does not make the skipped frame
defined behavior. The trap-safe form is an explicit lock/unlock with a defer
record, not a non-trivial guard object:

```cpp
hostMutex.lock();
auto token = trap->defer(unlockThunk, &hostMutex);
callIntoWasmThatMayTrap();
trap->dismiss(token);
hostMutex.unlock();
```

| Resource | Safe with a trap-cleanup defer? | Why |
|---|---|---|
| host `std::mutex` held across a trapping call | yes, with explicit `lock`/`unlock` and a defer thunk, provided the mutex object is outside the skipped frame or trivially destructible | release is an external action; do not use `std::lock_guard` in the skipped frame |
| per-call host-reference arena | yes, if the arena is owned outside the skipped frame and the thunk only drops roots | aligns with `HostBridge` per-instance ownership; see §9 |
| OS file descriptor / mapping / socket | yes | cleanup is a syscall/handle operation, not a C++ destructor |
| `std::unique_ptr` local in a skipped frame | **no** | its destructor is skipped; UB is already in play |
| `std::string` / `std::vector` / `std::function` local in a skipped frame | **no** | same non-trivial-destructor problem |
| `std::lock_guard` / `std::unique_lock` / `std::scope_exit` in a skipped frame | **no** | the cleanup object's own destructor is the mechanism, and it is skipped |
| guest-side Wasm lock or semaphore | **not by this mechanism** | the engine does not track guest lock ownership; this needs a module/engine protocol |
| instance-wide host roots | not enough on its own | `HostBridge::instanceDestroying` and a per-instance arena remain the lifetime backstop |

A defer thunk fixes release on the trap path; it does not make it safe to hold a
non-recursive host mutex across an export whose import re-enters it. That remains
a lock-ordering problem, not a lifetime problem.

## 7. A concrete current gap: a trapping start function

The defer discussion is not only theoretical. `ModuleInstantiator::instantiate`
currently constructs a `std::unique_ptr<ModuleInstance>` and then calls the
module's start function:

```cpp
auto instance = std::make_unique<ModuleInstance>(module, resolver, hostBridge);
// ... type translation, declaration, compilation, segments ...
if (module.hasStartFunction)
    callStartFunction(*instance, module.startFunctionIndex, instance->internals);
return instance;
```

A trap in the start function (or in a native import it calls) longjmps to the
embedder's `TrapScope`, skipping the rest of `instantiate`. Consequently:

- the local `std::unique_ptr` destructor does not run;
- `~ModuleInstance` does not run;
- `HostBridge::instanceDestroying` does not run;
- host references retained during construction can leak, and the partially
  constructed instance is orphaned;
- strictly, the skip of a non-trivial destructor is already undefined behavior,
  so this is not merely a memory leak.

The test harness treats a trapping start as the expected
`assert_uninstantiable` outcome (`Test/WastScript.cpp`), which is correct, but
it does not check that the instance was destroyed or that `instanceDestroying`
ran. That missing assertion is why the leak is not visible in the suite.

A defer stack alone is not a complete fix here. It could release external
resources, but the `unique_ptr` destructor is still skipped. The minimal correct
shape is a normal C++ catch boundary around the start call:

1. do not longjmp across the frame that owns the instance;
2. catch the trap in a nested `TrapScope` at a point where the instance holder
   is still a valid C++ local;
3. destroy/reset the instance normally (which invokes
   `HostBridge::instanceDestroying`);
4. re-raise the trap to the outer scope, or return a controlled instantiation
   failure.

The holder itself must remain trivially destructible or live in a frame that is
not skipped. A `std::unique_ptr` in the same frame as the nested `setjmp` is
fine only if it is constructed before `setjmp()` and not modified between
`setjmp()` and the jump; that is a constraint on how the fix is written, not a
free pass for arbitrary non-POD locals.

This start-function gap is independent of the JS adapter and should be fixed
regardless of whether the generic cleanup stack is accepted.

## 8. Alternatives considered

| Option | What it buys | Cost / risk | Verdict |
|---|---|---|---|
| Keep the current POD-only rule | no new runtime mechanism; the existing contract is honest | host locks/arenas still leak or deadlock on a trap; the start-function leak remains | status quo, not sufficient for a robust embedder |
| C++ scope guards / `std::scope_exit` | familiar RAII syntax | destructors are exactly what `longjmp` skips; no improvement | does not solve it |
| Go-style fixed-capacity cleanup stack | explicit trap-time release of host locks, arenas and handles; no JIT/ABI change | normal-path dismiss discipline; fixed cap; callbacks must be no-throw/non-reentrant; still cannot make non-trivial locals safe | **recommended pragmatic option** |
| C++ exceptions with unwind tables | real C++ unwinding and RAII | generated frames have no unwind tables; would need a documented ABI and CFI/personality support in every backend; exception paths are slower and harder to keep trap-compatible | not recommended now |
| Status-return traps (no `longjmp`) | every native call returns normally, so ordinary RAII works; easiest to reason about | invasive codegen/ABI redesign: every helper and generated branch needs an error path; tail calls, imports, multi-value and traps all change | clean long-term direction, too large for this step |
| Engine-owned lock/resource registry | can release/poison host and guest locks after an agent dies; supports cancellation | needs per-agent ownership tracking, poisoning semantics, and a policy for partially executed critical sections; multi-agent scope | separate design item, see [`THREADSAFETY.md`](THREADSAFETY.md) §10.1 |
| `setjmp` per import with full local cleanup | would let a native import catch and clean its own locals | re-raising after cleanup must not be caught again; setjmp clobber and frame lifetime rules are easy to get wrong; every import pays a setjmp | narrower alternative, not a general replacement |

## 9. Relation to `HostBridge` and host adapters

[`ABI.md`](ABI.md#external-host-adapter-optional-hostbridge) defines
`HostBridge::instanceDestroying` as the per-instance teardown hook. It runs at
the start of `~ModuleInstance` and is the right place to drop an adapter's
per-instance handle arena. It is **not** a per-call cleanup hook, and it cannot
run if the instance destructor is skipped by a trap (see §7).

A host adapter that can run arbitrary host code from an import needs three
levels of ownership:

| Level | Mechanism | Current status |
|---|---|---|
| per instance | `HostBridge::instanceCreated` / `instanceDestroying` and a per-instance arena | shipped |
| per call | a per-call handle/root arena cleared on normal return and on trap | not shipped; this note's defer stack would make it possible |
| per reference | `HostBridge::references.retain` / `release` through `refClone`/`refStore`/`refDrop` barriers | shipped, but only for counted reference slots |

A per-call arena is especially useful for a JS/Lua/Python adapter: a host value
temporarily exposed through `externref` does not need to become a long-lived
root if the adapter can release the call's temporary handles at the end of the
call, including when the call traps. The cleanup thunk would reset the arena;
it must not call `WASM::trap`, `refDrop`, or re-enter Wasm while trap handling
is in progress. The engine must not assume `instanceDestroying` will repair
per-call state after a trap.

`HostBridge` callbacks themselves must not throw or trap
([`WasmHostBridge.hpp`](../WasmBase/WasmHostBridge.hpp)). A host language
runtime that uses its own `longjmp`/exception mechanism must catch and translate
that at the adapter boundary; otherwise it can jump over engine frames and
bypass the trap state entirely.

Nothing in this note adds a JavaScript or host-language dependency to the
engine. The cleanup contract is a C++ API; the language-specific root/handle
policy belongs to the adapter.

## 10. Implementation plan (not committed)

| Milestone | Work | Done when |
|---|---|---|
| **TC-0 — This note** | Record the design, constraints, current gap and alternatives. No runtime change. | This note is linked from `SAFETY.md`, `THREADSAFETY.md`, `ABI.md` and the README index. |
| **TC-1 — Start trap cleanup** | Add a normal C++ catch boundary around the start function in `ModuleInstantiator::instantiate`; destroy the partial instance and re-raise/return the trap. | A start-function trap invokes `~ModuleInstance` and `HostBridge::instanceDestroying`; no `unique_ptr` is skipped. |
| **TC-2 — Cleanup stack** | Add a fixed-capacity LIFO cleanup stack to the per-thread trap state, `TrapScope::defer`/`dismiss`, `currentTrapScope()`, and a `trap()` drain loop. | A native import can release a host mutex/arena on both normal return and trap; LIFO and nested scopes are tested. |
| **TC-3 — Host per-call arena** | Let the adapter use the cleanup stack to reset a per-call host-root arena; document it alongside `HostBridge`. | A trapping import releases the call's host handles; instance teardown remains the backstop. |
| **TC-4 — Multi-agent audit** | Decide whether cleanup state is per host thread or per agent; add ownership/poisoning policy if agents can share a thread. | The answer is documented in `THREADSAFETY.md`; no per-thread assumption leaks into the agent model. |

## 11. Test plan

| Test | What it proves |
|---|---|
| Native import arms a mutex, registers a defer, then traps | the unlock thunk runs exactly once before the embedder sees the trap |
| Same import returns normally | `dismiss` removes the record; the thunk does not run later |
| Two nested defers | execution order is LIFO |
| Nested `TrapScope`s | an inner trap drains only the inner snapshot; outer cleanup survives |
| Missing/duplicate dismiss in a debug build | the contract violation is detected rather than silently leaking |
| Cleanup thunk calls `WASM::trap` or Wasm | the engine aborts or otherwise refuses recursive dispatch; it never re-enters JIT frames |
| Start function traps after a fake host adapter retains a reference | `~ModuleInstance` runs, `instanceDestroying` runs, the host count returns to zero |
| Start function traps before any host reference exists | the partial instance is destroyed and the trap reaches the embedder with the original kind |
| Existing `assert_uninstantiable` case | the expected start trap still reports `TrapKind` correctly |
| Full suite under ASan/valgrind where available | the start-trap fix and defer records introduce no use-after-free or leak |

The current suite has one trapping start case (`start.wast`) in
`assert_uninstantiable`; it should be extended with an adapter-level cleanup
assertion, not merely a "the trap happened" check.

## 12. Open questions

- **Normal-path ergonomics.** Is explicit `dismiss` acceptable, or is a
  documented non-trivial guard usable only in frames that cannot be skipped?
  The latter is easy to misuse; Go's compiler support is exactly what C++ lacks.
- **Fixed capacity.** What is the cap, and what happens on overflow? A trap that
  cannot run its cleanup is worse than a normal-path allocation failure.
- **Cleanup failure.** If a thunk must not throw/trap, should a failure be
  `std::abort`, a recorded secondary error, or a poisoned instance? There is no
  safe general recovery path.
- **Reentrancy.** Can a cleanup callback release a host lock that another host
  thread needs to make progress, and can that deadlock trap handling? The
  adapter must define its lock ordering.
- **Host callbacks in cleanup.** If a host-language runtime's release hook can
  itself fail, the adapter must decide before registering it.
- **Guest locks.** Nothing in this note releases a guest lock. A future engine
  needs either a Wasm-level cleanup/`finally` path or an engine-owned lock
  registry; a host defer stack cannot see guest mutexes.
- **Multi-agent.** Per-thread cleanup state is sufficient today, but an agent
  sharing a host thread needs its own trap target and cleanup stack.

## 13. References

- [`SAFETY.md`](SAFETY.md) §2.4–§2.6 — traps vs aborts, RAII for locks, and the
  POD-only trap path this note starts from.
- [`THREADSAFETY.md`](THREADSAFETY.md) §5.6 and §10.1 — host mutex/RAII caveats,
  trap/lock ownership, and the open multi-agent questions.
- [`ABI.md`](ABI.md) — `TrapScope` usage, the `Callable` layout, native imports,
  and the external `HostBridge` adapter.
- `WasmBase/WasmTrap.hpp` / `WasmBase/WasmTrap.cpp` — `TrapScope`, `TrapState`,
  `trap()` and the tail-call reset.
- `WasmBase/WasmModuleInstance.cpp` — `ModuleInstantiator::instantiate` and the
  start-function path.
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) §4.3 — the
  opt-in host bridge that a per-call host arena would extend.
- Go language reference, “Defer statements” and “Handling panics” — the
  inspiration, not an ABI dependency.
