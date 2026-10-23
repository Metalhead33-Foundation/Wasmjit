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

## 14. Revision 2 by Claude — Confine `longjmp` to generated code (candidate)

Status: candidate addendum to this note. **No runtime change is implemented.**
All identifiers below (`ImportResult`, `TrapRequest`, `callGuarded`, the debug
flag) are proposals; none exist today. Signatures are illustrative and must be
reconciled with the `Callable` layout in [`ABI.md`](ABI.md) before anything is
written. The code sketches were drafted without access to `WasmTrap.cpp` or the
backends, so every claim about current behavior should be checked against them.

### 14.1 The idea

Sections 5–6 try to make a skipped frame *survivable* with a cleanup stack.
This addendum instead makes most frames *unskippable*: `longjmp` is kept, but
the set of frames it may cross is shrunk to something small and auditable.

> **Rule R2.** A trap may skip only generated frames, engine runtime helpers on
> the audited list (§14.5), and engine-provided import thunks. No other frame
> sits between a trap point and its `TrapScope`.

Everything outside that set may use ordinary C++: `std::lock_guard`,
`std::unique_ptr`, `std::string`, exceptions. That includes native import
bodies, `ModuleInstantiator::instantiate`, and the embedder.

| Frame | May be skipped by a trap? | Non-trivial locals allowed? |
|---|---|---|
| Generated Wasm code | yes | no (nothing to destroy) |
| Audited runtime helper (§14.5) | yes | **no**, POD-only as in SAFETY.md §2.6 |
| Import thunk (engine-provided) | yes | **no**, POD-only |
| Native import **body** | **never** | **yes** |
| `callGuarded` (§14.3) | never; it is the jump target | only trivially destructible locals between `setjmp` and the jump |
| `instantiate`, embedder code | never | yes |

The property that makes this work is ordering: an import body *returns* a status
and is gone before the trap is raised, so its destructors have already run when
`longjmp` happens.

### 14.2 Import contract

A native import body never calls `WASM::trap()`. It returns a trivially copyable
result:

```cpp
struct TrapRequest {                 // trivially copyable
    WASM::TrapKind kind;
    char           message[96];      // truncated, NUL-terminated, inline
};

struct ImportResult {                // trivially copyable
    bool        trapped;
    TrapRequest trap;                // valid iff trapped
};

// Body shape is illustrative; the real signature follows the Callable layout.
ImportResult myImport(VMContext& ctx, const Value* args, Value* results);
```

The message is copied inline on purpose. A `std::string` or `const char*` into
the body's own storage would dangle by the time the thunk reads it.

The engine supplies the thunk between generated code and the body:

```cpp
// POD-only frame. Skippable by design.
void importThunk(VMContext* ctx, const Value* args, Value* results) {
    ImportResult r = runImportBody(ctx, args, results);   // body frame returns first
    if (r.trapped)
        WASM::trap(r.trap.kind, r.trap.message);           // copies into TrapState, then longjmps
}
```

`ImportResult` is trivially destructible, so the thunk frame is legal to skip.
`WASM::trap` copies the message into thread-local `TrapState` before jumping, so
no pointer into a dead frame survives.

Body obligations:

- must not call `WASM::trap`, `longjmp`, or throw across the thunk. An exception
  escaping a body is caught by `runImportBody` and converted to a trap (or
  aborts, see §14.8);
- may hold any C++ resource, including across a re-entrant call into Wasm
  (§14.3);
- reports a failure only by returning `trapped = true`.

### 14.3 Scoped re-entry: `callGuarded`

An import that calls back into Wasm must not call generated code directly,
because a trap there would skip the import body. It uses a guarded call that
arms its own `TrapScope`:

```cpp
struct CallResult {                  // trivially copyable
    bool        ok;
    TrapRequest trap;                // valid iff !ok
};

[[gnu::noinline]]
CallResult callGuarded(const Callable& fn, const Value* args, Value* results) {
    WASM::TrapScope scope;           // saves previous scope + cleanup depth BEFORE setjmp
    if (setjmp(scope.buffer) == 0) {
        invoke(fn, args, results);   // may longjmp back here
        return CallResult{true, {}};
    }
    CallResult r{false, {}};         // declared after setjmp: not clobbered
    r.trap.kind = scope.kind();
    copyTruncated(r.trap.message, scope.message());
    return r;                        // ~TrapScope restores the outer active scope
}
```

Points that matter:

- The frame containing `setjmp` is the jump *target*, not a skipped frame, so
  `TrapScope`'s own destructor is fine there. Only frames strictly between the
  trap and the target are skipped.
- No local written after `setjmp()` is read after the jump except `r`, which is
  constructed on the trap branch itself. This avoids the indeterminate-value
  rule in §5.4.
- The scope is restored before `callGuarded` returns. The caller's later
  `ImportResult{trapped}` therefore re-raises to the *outer* scope, never back
  into the inner one.
- `[[gnu::noinline]]` is belt and braces. Compilers generally refuse to inline
  `setjmp` callers, but this must not depend on that.

An import that re-enters Wasm looks like:

```cpp
ImportResult myImport(VMContext& ctx, const Value* args, Value* results) {
    std::lock_guard lock(hostMutex);                 // legal: this frame is never skipped
    auto buf = std::make_unique<Scratch>();          // legal
    CallResult r = callGuarded(callback, args, results);
    if (!r.ok)
        return ImportResult{true, r.trap};           // lock and buf released by normal return
    return ImportResult{false, {}};
}
```

### 14.4 `instantiate` and the start function

The start function is the same pattern with the instantiator as the guarded
caller:

```cpp
auto instance = std::make_unique<ModuleInstance>(module, resolver, hostBridge);
// ... type translation, declaration, compilation, segments ...
if (module.hasStartFunction) {
    CallResult r = callGuarded(startCallable(*instance), nullptr, nullptr);
    if (!r.ok) {
        instance.reset();            // ~ModuleInstance -> HostBridge::instanceDestroying
        // contract decision (TC-1): re-raise via WASM::trap, or throw a C++ instantiation error
    }
}
return instance;
```

This supersedes the "catch boundary" wording in §7 and removes the need for a
bespoke nested `TrapScope` in `instantiate`. The open contract question (re-raise
vs. convert to an exception) is unchanged and belongs to TC-1.

### 14.5 The audited helper list

Rule R2 is only as strong as the list of things a trap may cross. The list is
closed and maintained in one place (proposed: a table in
[`SAFETY.md`](SAFETY.md) §2.6, mirrored by one namespace such as `WASM::rt`):

- helpers called directly from generated code (`memory.grow`, table operations,
  reference-count barriers `refClone`/`refStore`/`refDrop`, bounds-failure
  paths, and similar);
- the engine-provided import thunks of §14.2.

Each entry is POD-only. A helper that needs non-trivial locals is not allowed to
grow them; it must instead return a status that the generated code branches on,
or be split so the non-trivial part runs in an unskippable frame.

This is a review-enforced list. There is no reliable compiler check for "no
non-trivial locals in this function", so §14.6 adds a runtime tripwire.

### 14.6 Debug tripwire

A debug-only thread-local flag `inImportBody` catches the two violations that
matter most:

- `runImportBody` sets it before calling the body and clears it after.
- `callGuarded` saves it, clears it for the duration of the guarded call, and
  restores it on exit, on both the normal and the trap branch.
- `WASM::trap()` asserts `!inImportBody`.

A body that calls `WASM::trap()` directly fires the assert. So does a body that
reaches generated code through a raw function pointer instead of `callGuarded`,
because a trap there runs with the flag still set. These are exactly the
mistakes that silently reintroduce the skipped-destructor hole.

The flag does not detect a non-trivial local in an audited helper; that remains
a review item.

### 14.7 Effect on the rest of this note

| Section | Effect |
|---|---|
| §3 table, "Wasm `longjmp` trap" row | The "native path must be POD-only" clause narrows to "generated code, audited helpers and thunks". |
| §5 defer stack | Demoted. The main use cases (host mutex or per-call arena held across a trapping call) now live in an import body that cannot be skipped. Remaining use: resources held by an audited helper across a trap, which should be rare. |
| §6 table | Rows for `std::lock_guard` and `std::unique_ptr` become "yes, in an import body"; they stay "no" in a skipped frame. |
| §7 | Fixed by §14.4 using `callGuarded`. |
| §8, "`setjmp` per import" row | Cost objection no longer applies: imports do not pay a `setjmp`. Only an import that *re-enters Wasm* pays one, per re-entry. |
| §9 per-call arena | An adapter's per-call arena can be an ordinary RAII object in the import body. |
| §12 "Normal-path ergonomics" | Largely moot: no `dismiss` discipline on the common path. |

Revised milestones, replacing the §10 table if this is accepted:

| Milestone | Work | Done when |
|---|---|---|
| **TC-1 — Start trap cleanup** | `callGuarded` and the §14.4 shape in `instantiate`. | A start-function trap runs `~ModuleInstance` and `instanceDestroying`. |
| **TC-1b — Import contract** | `ImportResult`/`TrapRequest`, import thunk, debug tripwire; migrate existing native imports off direct `WASM::trap()`. | No in-tree import calls `WASM::trap`; tripwire enabled in the test build. |
| **TC-1c — Helper audit** | Write the §14.5 list and review each entry. | List committed in SAFETY.md. |
| **TC-2 / TC-3** | Cleanup stack and host arena. | Only if a concrete audited-helper case needs them. |
| **TC-4 — Multi-agent** | Unchanged; `callGuarded` and the flag move to per-agent state. | Documented in THREADSAFETY.md. |

### 14.8 Costs and risks

- **API break.** Existing native imports that call `WASM::trap()` must return
  `ImportResult` instead. Out-of-tree embedders break at compile time, which is
  the desired failure mode.
- **Leak through raw calls.** The guarantee holds only if every path from an
  import body into generated code goes through `callGuarded`. Do not expose a
  raw call API to import authors.
- **Exceptions escaping a body.** `runImportBody` must either translate them to
  a trap or `std::abort`. Letting one propagate into the thunk and generated
  frames is undefined (no unwind tables).
- **`setjmp` cost per re-entry.** One `setjmp` per host→Wasm re-entry. Check
  whether the implementation saves the signal mask (`_setjmp` or
  `sigsetjmp(buf, 0)` avoid it). The cost is paid only by importers that call
  back into Wasm.
- **Unchanged problems.** Guest locks (§12) and lock-ordering across re-entrant
  imports are not addressed by this addendum.

### 14.9 Additional tests

| Test | What it proves |
|---|---|
| Import holds `std::lock_guard`, re-enters Wasm via `callGuarded`, inner call traps | The mutex is released, the outer trap reaches the embedder with the original kind and message |
| Same, with a `std::unique_ptr` whose destructor increments a counter | The counter is incremented exactly once before the embedder sees the trap |
| Import calls `WASM::trap()` directly (debug build) | The tripwire asserts |
| Import calls generated code without `callGuarded` and it traps (debug build) | The tripwire asserts |
| Nested `callGuarded` two levels deep, inner trap | Only the inner scope catches; the outer scope is restored and sees the re-raised trap |
| Trap message longer than 95 characters | It is truncated and NUL-terminated, not overrun |
| Body throws a C++ exception | Converted to a trap or aborts, per the chosen policy; never unwinds into generated frames |
| Start function traps | As §11, via `callGuarded` |

### 14.10 Open questions

- **Message buffer size.** Is 96 bytes enough for the existing `trap()` detail
  strings?
- **Tail-call trampoline state.** `trap()` resets the pending target today. A
  `callGuarded` entered from an import that was itself reached through the
  trampoline needs the pending target saved and restored. Verify against the
  trampoline implementation.
- **Exception policy.** Convert an escaping body exception to a trap, or abort?
- **Embedder entry point.** Should the embedder-facing call also become
  `callGuarded`, leaving a raw `TrapScope` as the low-level primitive only?
- **Value-returning helpers.** Which audited helpers can return a status
  cheaply, and which would need generated-code changes to branch on one?

  ## 15. Alternatives to `setjmp`/`longjmp` — and why they are not adopted yet, by DeepSeek

§8 lists "status-return traps" as the clean long-term direction and rates the other
alternatives as too costly. That framing is correct but undersells one point: the
status-return design is not merely an alternative mechanism, it is the only listed
option that **removes the POD-only rule instead of working around it**. This section
states that distinction explicitly, adds the per-import `setjmp` boundary as a
considered-but-not-recommended variant, and records why none of these options is
adopted in the current step.

### 15.1 The cure/workaround distinction

The existing mechanisms fall into two classes:

- **Workarounds** that keep `longjmp` and give native code an explicit escape hatch
  for *external* state: the defer stack (§5) and the per-import `setjmp` boundary
  (§14.3). These make host locks, arenas and handles releasable on the trap path.
  They do not restore C++ lifetime rules; a non-trivial local in a skipped frame is
  still undefined behavior.
- **Cures** that replace the non-local exit and therefore make ordinary RAII valid
  again in frames that currently can be skipped: status-return traps (§14.2) and
  C++ exceptions with unwind tables (§8, not recommended).

The distinction matters for planning. A workaround bounds the damage of the current
model; a cure changes the model. TC-1 (§7) is neither — it removes one concrete UB
site under the current model and is required regardless of which direction is later
taken.

### 15.2 Status-return traps (the cure)

Every native call returns a trap status (a `TrapResult` value or a flag in
`VMContext`); codegen emits an explicit branch to a shared error epilogue at every
trapping instruction and every call site.

What it buys:

- no non-local exit, so C++ destructors run on every path, including native imports
  and engine-internal frames;
- native imports may hold ordinary `std::lock_guard`, `std::unique_ptr`, `std::string`
  and other non-trivial locals without qualification;
- the defer stack, the POD-only rule and `TrapScope` all become unnecessary for
  cleanup purposes;
- interaction with a future Wasm exception-handling proposal is a single design
  problem rather than two coexisting unwinding mechanisms.

What it costs:

- codegen rework in every backend: every trapping instruction needs an error branch
  and a shared epilogue that unwinds the Wasm operand stack and frame layout;
- an import ABI change: every native import signature gains an out-parameter or a
  sentinel, the `HostBridge::Callable` layout changes, and every adapter is rewritten;
- tail calls must propagate status through the trampoline, not just return;
- a per-call-site branch on the hot path (predict-not-taken in practice, but not
  free) and additional code size for the epilogues;
- coexistence with any future Wasm EH design, which must be resolved together.

Assessment: this is the design that would have been chosen had the trap path been
specified with C++ lifetime rules as a constraint. It is not a patch; it is a
codegen and ABI redesign spanning weeks to months and touching every backend and
every adapter. It should be tracked as the principled long-term direction and gated
on a concrete trigger (see §14.4), not scheduled as cleanup work.

### 15.3 Per-import `setjmp` boundary (narrow workaround)

Each native import that wants cleanup arms its own `TrapScope` around the call back
into Wasm, catches the trap locally, releases its own non-trivial locals through
ordinary C++ control flow, and re-raises.

What it buys:

- an import can hold non-trivial locals in *its own* frame, because it catches the
  trap before the `longjmp` reaches it;
- per-import cleanup does not require the defer stack or any new engine API.

What it costs:

- every participating import pays a `setjmp` — tens of cycles on typical ABIs, and a
  compiler barrier; this is a real tax on hot imports;
- the re-raise must not be caught by the same scope again; the doc's §8 already flags
  that re-raise-after-cleanup and `setjmp` clobber rules are easy to get wrong;
- it does not address engine-internal frames such as the start-function path (§7), so
  TC-1 remains required;
- it is still `longjmp`, with a narrower blast radius.

Assessment: interchangeable with the defer stack for the "release external state on
the trap path" use case. The trade is `setjmp` cost and re-raise discipline against
the defer stack's explicit-dismiss discipline and fixed capacity. Neither is
obviously better; the choice belongs with the first concrete import that needs it
(TC-2 or a per-import variant, decided then).

### 15.4 C++ exceptions with unwind tables

Discussed in §8 and not recommended now. Worth recording the reason alongside
status returns: the effort is comparable to status returns (CFI and a personality
routine in every generated frame, backend-specific), the debuggability of
exceptions crossing JIT frames is materially worse, and the result is only worth it
if Wasm exception handling is being added anyway. If Wasm EH is added, this option
should be re-evaluated together with status returns rather than independently.

### 15.5 Decision and triggers

The current decision is unchanged: TC-1 now, TC-2 only if a native import needs it,
do not replace `longjmp` for this work. The amendment is to record the gating
conditions for revisiting the cure:

- **Revisit status-return traps** when any of the following holds:
  (a) an engine-internal frame other than the start function needs to hold a
  non-trivial local across a trap-capable call; (b) a Wasm exception-handling
  proposal is adopted, so a single unwinding design is required; (c) the defer
  stack's discipline proves to be a recurring source of bugs in review.
- **Revisit per-import `setjmp`** at TC-2 time, as an alternative to the defer stack
  for import-local cleanup. The two should be compared on the first real import, not
  chosen abstractly.
- **Revisit C++ exceptions** only together with Wasm EH, not independently.

The POD-only rule in [`SAFETY.md`](SAFETY.md) §2.6 remains in force under the current
model. This section does not relax it; it records why the rule exists and what would
have to change for it to be removed.

### 15.6 Amendment to the §8 comparison table

The §8 table is retained as written. Add the following rows and footnote, or replace
the corresponding cells if the table is revised:

| Option | What it buys | Cost / risk | Verdict |
|---|---|---|---|
| Per-import `setjmp` boundary | import-local cleanup of non-trivial locals in the import's own frame | `setjmp` cost on every participating import; re-raise discipline; does not fix engine-internal frames | **narrow workaround**, interchangeable with the defer stack at TC-2 |
| Status-return traps | removes the POD-only rule; ordinary RAII everywhere, including engine frames | codegen and import-ABI redesign; hot-path branch; tail-call and future-EH interaction | **cure; long-term direction**, gated per §14.5 |

Footnote to the table: "Workarounds (defer stack, per-import `setjmp`) bound the
damage of the current `longjmp` model. Cures (status returns, C++ exceptions)
change the model. TC-1 is independent of both and is required under the current
model."

### 15.7 Implementation plan amendment

Add to the §10 milestone table:

| Milestone | Work | Done when |
|---|---|---|
| **TC-1 — Start trap cleanup** *(unchanged)* | Nested C++ catch boundary around the start function; POD-only helper holding the `setjmp`; snapshot the trap kind/message before destroying the instance; reset the holder before re-raising. | A start-function trap invokes `~ModuleInstance` and `HostBridge::instanceDestroying`; no `unique_ptr` is skipped; the outer embedder sees the original `TrapKind`. |
| **TC-2 — Import cleanup** *(revised)* | Choose between the defer stack (§5) and the per-import `setjmp` boundary (§14.3) on the first concrete import that needs trap-time cleanup; document the choice and the rejected alternative. | A native import releases a host mutex/arena on both normal return and trap; the chosen mechanism's discipline is tested (LIFO, nested scopes, double-dismiss or double-catch). |
| **TC-5 — Trap-model review** *(new)* | Record the cure/workaround distinction, the gating conditions in §14.5, and the relationship to any Wasm EH work. No runtime change. | This section is linked from `SAFETY.md` §2.6 as the recorded rationale for the POD-only rule. |

TC-2's "done when" is deliberately unchanged in substance; only the mechanism choice
is deferred to the first real caller. This keeps the defer stack from being built
speculatively.

### 15.8 Test plan amendment

Add to the §11 table:

| Test | What it proves |
|---|---|
| Start-function trap with a fake adapter that retains a reference during construction | `~ModuleInstance` runs, `instanceDestroying` runs, the host reference count returns to zero, and the embedder observes the original `TrapKind` (TC-1). |
| Start-function trap whose `instanceDestroying` callback reads thread-local trap state | the snapshot-before-teardown ordering in TC-1 is correct; the propagated kind is not corrupted by host code running during teardown. |
| TC-2 mechanism chosen: normal return and trap on the same import | the import's cleanup runs exactly once on both paths (dismiss/double-catch discipline). |


## 16 Alternatives to `setjmp`/`longjmp` according to Gemini

There are two primary realistic alternatives to `setjmp`/`longjmp` for implementing traps in a C++ WebAssembly engine. Both come with clear trade-offs depending on whether you want zero execution overhead or idiomatic, exception-safe C++ code.

---

### Alternative 1: Explicit Status-Return / Result Propagation

Instead of unwinding or jumping, every compiled Wasm function and native helper returns an explicit status code (or returns a `Result<T, Trap>` struct).

```cpp
// Generated code / host helper pattern
TrapStatus wasm_func_add(uint32_t a, uint32_t b, uint32_t* out_result) {
    if (will_overflow(a, b)) {
        return TrapStatus::IntegerOverflow;
    }
    *out_result = a + b;
    return TrapStatus::Ok;
}

TrapStatus calling_func(...) {
    uint32_t res;
    TrapStatus status = wasm_func_add(x, y, &res);
    if (status != TrapStatus::Ok) {
        return status; // Standard return: destructors run naturally!
    }
    ...
}

```

* **How it works:** Traps are treated as regular control flow returns. Every caller checks if the callee trapped and bubbles the error up the stack.
* **Pros:**
* **100% C++ Standard Compliant:** Standard C++ destructors, RAII, `std::unique_ptr`, `std::lock_guard`, and scope guards run automatically on every frame during the return sequence.
* **Zero Platform Specifics:** Requires no OS-specific exception handling, assembly trampolines, or C-standard caveats (`setjmp` local variable invalidation).


* **Cons:**
* **Code Size & Branch Overhead:** Every call site in generated JIT/AOT code must include a branch checking for a trap status.
* **Calling Convention Redesign:** Requires modifying native function signatures, which complicates tail-call optimizations and multi-value returns.



---

### Alternative 2: Platform-Native Signal/Exception Unwinding (SEH / Signals)

Instead of manually jumping via C standard library calls, rely on OS/architecture-level structured exception handling or signals combined with standard C++ exceptions or DWARF unwind tables.

```cpp
// Native import / runtime helper throws a dedicated C++ exception
void WASM::trap(TrapKind kind, const char* msg) {
    throw WasmTrapException(kind, msg);
}

// Host boundary
try {
    callIntoWasm();
} catch (const WasmTrapException& e) {
    // Standard C++ unwinding ran all intermediate destructors safely
    handleTrap(e);
}

```

* **How it works:** Traps throw a standard C++ exception (or trigger a hardware fault handler like `SIGSEGV`/`SIGFPE` that translates into a thrown C++ exception via OS support like Windows SEH `_set_se_translator` or POSIX `sigaction` with `libunwind`).
* **Pros:**
* **Zero-Cost Happy Path:** Normal execution runs at full speed without checking return status branches at every function boundary.
* **RAII Restored:** C++ exception handling (`throw` / `catch`) explicitly invokes destructors for all automatic objects in skipped frames as it unwinds.


* **Cons:**
* **Unwind Table Requirement:** Every JIT-generated frame **must** generate valid unwind tables (e.g., `.eh_frame` on Linux/macOS or `.pdata`/`.xdata` on Windows PE/COFF) so the C++ exception personality routine knows how to step through JIT code.
* **Heavy Toolchain/Backend Dependency:** If your JIT compiler backend does not emit CFI (Call Frame Information) directives or platform unwind metadata, throwing a C++ exception across generated frames will immediately abort or crash.



---

### Comparison Matrix

| Approach | Destruction / RAII | Happy-Path Overhead | JIT Backend Complexity | Embedder Ergonomics |
| --- | --- | --- | --- | --- |
| **`setjmp` / `longjmp**` *(Current)* | **Broken** (UB for non-POD) | Zero | Low | Poor (Requires manual `defer` / nested scopes) |
| **Status-Return Propagation** | **Built-in** (Standard returns) | High (Branch after every call) | Medium (ABI changes) | High (Standard C++ control flow) |
| **C++ Exceptions / Unwind Tables** | **Built-in** (Standard unwinding) | Zero | High (Must emit unwinding metadata) | High (Standard `try`/`catch`) |

---

### Recommendation

1. **If you want to keep the JIT backend simple:** Transitioning to **Status-Return Propagation** is the most architecturally sound choice. It eliminates `longjmp` entirely and restores 100% standard C++ lifetime rules without needing unwind metadata in JIT frames.
2. **If happy-path execution speed is non-negotiable:** Stick with `setjmp`/`longjmp` for JIT frames, but strictly isolate `setjmp` into tiny POD-only boundary helpers (like the TC-1 fix) and forbid non-POD C++ locals anywhere on trap-capable paths.


## 17 Alternatives to `setjmp`/`longjmp` according to Grok

`setjmp`/`longjmp` is convenient and historically common, but it is no longer the only (or even the preferred) approach in production Wasm engines. The problems you are hitting — skipped destructors, UB on non-POD locals, and the need for a secondary cleanup protocol — are exactly why others have moved away from it.

### Production precedent

Wasmtime is the clearest recent example. It used `setjmp`/`longjmp` for years, then **replaced** it (PR #11592, ~2025) with Cranelift’s exception-handling machinery. Reasons cited match your design note almost one-to-one:

- `longjmp` skips destructors (dangerous in Rust; same issue in C++).
- Rust does not understand functions that return twice.
- Dual unwinding mechanisms once the Wasm exception-handling proposal landed.
- Signal-handler subtleties.

They kept the desirable O(1) property for traps by storing the handler frame information in the host→Wasm trampolines instead of relying on a general `longjmp`.

SpiderMonkey has long used a related pattern: specialized entry trampolines + a throw stub that walks a maintained frame structure (or plants a sentinel) and returns to the trampoline, which then returns an exceptional result to the host. No general-purpose `longjmp` over arbitrary frames.

### Realistic alternatives for this engine

| Alternative | How it works | RAII / destructors | Invasiveness | Notes |
|-------------|--------------|--------------------|--------------|-------|
| **Trampoline + status return** (most practical near-term) | Only the host→Wasm entry trampoline is the non-local exit point. On trap it returns a status/error to the embedder. Inside Wasm + native helpers you either stay POD-only or propagate an explicit trap flag. | Full RAII outside the trampoline; POD-only (or explicit checks) inside | Medium — mainly changes the call boundary and a few hot helpers | Closest to what SpiderMonkey and post-2025 Wasmtime do. Fixes the start-function UB without a defer stack. |
| **Full status-return traps** | Every generated function and every trap-capable native helper returns a trap status (or uses an out-parameter / thread-local flag that is checked on every return path). No non-local jumps at all. | Perfect — ordinary C++ control flow | High — ABI, codegen, tail calls, multi-value, imports all change | The design note already calls this the “clean long-term direction.” |
| **C++ exceptions + unwind tables** | Generate proper CFI / personality routines so a C++ `throw` can cross JIT frames. Traps become exceptions (or a special uncatchable exception). | Perfect | High — every backend needs unwind support; exception paths are slower; ABI must be documented | Viable if you already want Wasm exception-handling proposal support. |
| **Reuse Wasm exception machinery for traps** | Once you have `try_table` / throw support (or an equivalent), treat traps as uncatchable exceptions that always unwind to the host entry point. | Good (at the host boundary) | Medium–high (requires the exception infrastructure first) | What Wasmtime ended up doing. |
| **Signal-based + structured return** | Use OS signals for some traps (bounds, stack overflow, etc.), then return from the signal handler to a known trampoline that does a normal status return. | Good at the boundary | Platform-specific complexity | Common historically; still used for some fault types. |

### Recommendation for this codebase

The lowest-risk realistic alternative that actually solves the RAII problem is the **trampoline + status return** approach:

1. Keep a single, well-defined non-local exit at the host→Wasm entry trampoline.
2. Make that trampoline return a normal C++ status (success / trap kind + message).
3. Inside the Wasm execution path, either:
   - stay with the current POD-only rule for frames that can still trap, or
   - gradually convert hot native helpers to explicit status returns.
4. The start-function path in `ModuleInstantiator::instantiate` then becomes ordinary C++: the nested call either returns normally or returns a trap status, the `unique_ptr` is destroyed by normal control flow, and `HostBridge::instanceDestroying` runs.

This is strictly better than bolting a Go-style defer stack onto `longjmp`. It does not require inventing a new cleanup protocol, does not leave non-trivial locals in skipped frames, and matches what mature engines have converged on.

Full status-return everywhere is cleaner still, but it is a larger project (exactly as the design note says). C++ exceptions with full unwind tables are attractive only if you are already planning to support the Wasm exception-handling proposal.

**Bottom line:** Yes, there is a realistic alternative that restores C++ lifetime rules without a secondary defer mechanism. The trampoline-boundary + status-return model is the pragmatic next step; pure `longjmp` over mixed C++/JIT frames is increasingly considered a liability rather than a feature.


## 18 Alternatives to `setjmp`/`longjmp` according to ChatGPT

Yes. And given that WasmJit's a C++ runtime whose generated machine code calls back into native C++, there are realistic alternatives that don't require you to surrender RAII or build a miniature Go runtime just to clean up after a trap.

The important question is how traps propagate from generated WebAssembly code into the host. You have several options, but they differ considerably in implementation cost, performance, and compatibility with your existing JIT backends.

### The four realistic alternatives

1\. Explicit status propagation

Best architectural alternative

Every generated function or helper returns a result that can indicate success or a trap. Callers propagate failures until a host boundary handles them.

- Ordinary C++ calls return normally, so RAII works.
- No `longjmp`, no C++ unwind tables required.
- Requires changes to generated control flow, helper ABIs, imports, and tail-call handling.

2\. C++ exceptions through generated code

Best RAII semantics

Make traps throw a dedicated exception and ensure the generated native frames support proper C++ unwinding.

- Real destructor execution during unwinding.
- Natural propagation through ordinary C++ helper frames.
- Requires compatible unwind metadata, personality routines, and exception-handling support for each relevant backend and platform. Your current generated frames don't have this support.

3\. Explicit trap checks at host boundaries

Keep the existing trap mechanism inside the generated execution region, but place carefully designed containment boundaries around host operations that require ordinary cleanup.

- Potentially a relatively small migration.
- Can fix specific ownership bugs without redesigning every generated function.
- Doesn't solve the general problem of non-trivial C++ objects in frames skipped by `longjmp`.

4\. Keep `setjmp`/`longjmp`, with strict boundaries

Retain the current fast, non-local trap mechanism but ensure jumps never cross frames containing objects whose destruction or lifetime semantics would be invalidated.

- Lowest disruption to the existing runtime.
- Preserves the current trap-oriented execution model.
- Keeps the burden of maintaining safe boundaries on the engine and native integrations.

### Which one makes sense for WasmJit?

For your runtime, I'd seriously compare status propagation against a hybrid design before committing to a generic defer stack.

| Criterion                   | `longjmp`                 | Status propagation             | C++ exceptions                    |
| --------------------------- | ------------------------- | ------------------------------ | --------------------------------- |
| Preserves normal C++ RAII   | No, across skipped frames | Yes                            | Yes, with proper unwind support   |
| Generated-code complexity   | Low                       | Higher                         | Higher                            |
| Per-operation overhead      | Low on normal path        | Depends on checks and ABI      | Depends on EH implementation      |
| Cross-platform backend work | Relatively small          | Mostly codegen and ABI work    | Significant unwind integration    |
| Tail-call integration       | Straightforward escape    | Requires propagation semantics | Requires unwind-compatible frames |
| Host-language integration   | Boundary restrictions     | Explicit error handling        | Exception-boundary restrictions   |

#### 1. Status propagation doesn't have to mean checking every instruction

This is the part I'd investigate carefully.

You could define an internal execution convention in which a generated function returns either a normal result or a trap status. Conceptually:

```
enum class ExecutionStatus : uint32_t {
    Ok,
    Unreachable,
    IntegerDivideByZero,
    MemoryOutOfBounds,
    IndirectCallTypeMismatch,
    // ...
};

ExecutionStatus executeFunction(
    VMContext* context,
    Result* result);
```

This is illustrative, not a proposed drop-in ABI. WebAssembly functions can have multiple results, and your existing calling convention may make a different representation preferable.

The key is that a trap propagates through function-call boundaries, not through every individual instruction.

For example:

1. A generated function detects an out-of-bounds access.
2. It branches to its trap-return path.
3. Each caller receives the failure and propagates it without executing subsequent Wasm instructions.
4. The host entry point receives the trap status and reports it through the existing trap-reporting interface.

The costs are additional control-flow paths, return-value handling, and propagation through direct calls, indirect calls, imports, and tail calls. You'd need to measure the normal-path overhead rather than assume it is negligible.

One possible optimization is to keep the common success path cheap and consolidate failure handling into a shared epilogue. Whether that works well depends on the native ABI and your JIT's generated code.

#### 2. C++ exceptions are viable, but they aren't automatically the easiest solution

There's a tempting idea here: simply make `WASM::trap()` throw an exception.

Unfortunately, throwing through JIT-generated machine code doesn't magically work just because the destination is C++. The generated frames need compatible unwind information and the necessary platform-specific exception-handling machinery. Without it, the exception cannot safely unwind those frames.

You could add that machinery to your backends, but you'd be taking on a substantial cross-platform implementation task. You'd also need a clearly defined policy for exceptions crossing native imports and host-language adapters.

If proper native unwinding is already available from a code-generation library you choose, this option becomes more attractive. Otherwise, I wouldn't select it merely to fix the current problem.

#### 3. There's a reasonable intermediate option

You don't necessarily have to choose between preserving `longjmp` forever and redesigning the whole runtime immediately.

Keep the existing trap mechanism while fixing the known start-function bug using a properly isolated trap-catching helper. Then investigate whether native imports actually need generic trap-time resource cleanup.

If they do, the defer stack remains a practical solution for explicitly managed external resources. If they don't, you may have avoided adding an entirely new subsystem.

That also leaves room to prototype status propagation behind a separate internal execution interface without committing to a new public ABI prematurely.
