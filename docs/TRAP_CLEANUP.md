# Trap Cleanup — Escaping the POD-Only Trap Path

Status: design note / implementation reference (revision 4 — **implemented**).
The trap path is a per-agent pending status that unwinds by *returning*:
generated code and engine helpers report and return a benign value, every caller
checks the slot, and the host boundary throws `WASM::Trap` once the generated
frames are gone. `longjmp` and `TrapScope` have been removed, the POD-only rule
is gone, and the optional deferred-cleanup stack ships as `WASM::defer`. §10
remains the feasibility study that selected this direction; §12 records what
shipped and what is still open.

**Revision 4:** TRAP-6 is implemented. The trap slot, pending exception,
tail-call state, deferred-cleanup stack, both depth counters and the stack limit
now live in `WASM::ExecutionState`, owned by an `Agent`
(`WasmBase/WasmAgent.hpp`); a host thread only holds the current-state pointer.
Where the historical sections below say "thread-local", read "the current
execution state" for the shipped tree.

Audience: anyone touching `WasmBase/WasmTrap.*`,
`WasmBase/WasmModuleInstance.*`, backend trap emission, native imports, or the
optional [`HostBridge`](ABI.md#external-host-adapter-optional-hostbridge) host
adapter.

**Revision 3 shipped** (details in §12):

- `WasmBase/WasmTrap.hpp` exposes a C-callable pending trap
  (`wasm_trap_slot()`, `reportTrap`, `wasm_trap_report`), a `WASM::Trap`
  exception, `checkTrap()`, and the explicit cleanup stack (`defer`,
  `dismissDeferred`, `runDeferredCleanup`).
- The LibJIT backend emits a per-function trap epilogue, a pending check after
  every helper call and call site (`emitPendingTrapCheck`), and a prologue check
  after the stack guard; the trampoline driver stops re-dispatching on a trap.
- Every in-tree helper that generated code reaches (stringref, array, atomic,
  memory/table bulk ops, GC allocation, exceptions, the stack guard) reports and
  returns instead of throwing or jumping — including the string/array helpers
  that previously held `std::vector` locals across a trap point.
- `WASM::callCallable`, `callCallableWithResults` and the start-function call
  convert the pending trap into `WASM::Trap`; `instantiate` therefore unwinds
  normally, so `~ModuleInstance` and `HostBridge::instanceDestroying` run (the
  old §7 gap). Nested calls (a host import re-entering Wasm) leave the trap
  pending instead of throwing into a generated frame.
- `Test/` uses try/catch and the harness trap result; `start.wast`'s
  `assert_uninstantiable` path runs through the same unwinding.
- The stringref/atomic/memory helper frames were the last POD-only sites; they
  now demonstrate the opposite property — their destructors run on the trap path
  (verified under AddressSanitizer on the trap-relevant test set).

§§4–§11 are retained as the design record: the option set, the deferred-cleanup
contract, the confinement alternative, and the per-backend study. Where they say
"candidate", read the shipped status above.

## 1. Summary and document map

- **A trap is reported, not thrown across generated code.** The generated frames
  carry no unwind tables, so neither a C++ exception nor a `longjmp` may cross
  them. A helper that fails calls `reportTrap()` and returns a benign value; the
  generated caller's pending check returns in turn.
- **The host boundary owns the exception.** `WASM::callCallable`, the
  start-function call and the harness convert the pending trap into `WASM::Trap`
  *after* the last generated frame has returned. Ordinary C++ unwinding — and
  therefore every destructor — applies to the engine's and embedder's frames.
- **RAII is unrestricted.** `std::unique_ptr`, `std::string`, `std::vector` and
  `std::lock_guard` may be live across a trap point; the early return runs their
  destructors. The POD-only rule is gone.
- **One narrow rule replaces it:** a helper reached from generated code must not
  throw (`SAFETY.md` §2.6). `WASM::trap()` is the host-facing spelling
  (report + throw) and is only valid where no generated frame is on the stack.
- **The pending slot is per agent.** Two host threads running Wasm cannot
  observe each other's traps, and neither can two agents sharing one host
  thread; generated code resolves the slot once per function through
  `wasm_trap_slot()` and loads `pending` inline thereafter.
- **Explicit cleanup remains available** for state no object owns:
  `WASM::defer(thunk, arg)` / `WASM::dismissDeferred(token)`, drained most-recent
  first by `reportTrap()`. It is the §5 design, shipped as a small utility, and
  it is *not* a substitute for RAII.
- **The start-function gap is fixed by construction:** a trapping start function
  throws out of `instantiate`, so the `std::unique_ptr<ModuleInstance>` is
  destroyed by C++ unwinding and `HostBridge::instanceDestroying` runs.
- **No backend can emit unwind information** (§10.1), which is why C++
  exceptions crossed generated frames nowhere and are not the trap mechanism.
- **What is still open:** guest-side cleanup (no Wasm `finally` on a trap), and
  true multi-agent execution. Per-agent trap state (**TRAP-6**) is implemented —
  see §12.1; what remains deferred is the execution model, not the state.

| Section | Question it answers |
|---|---|
| §2 | What is the trap contract today? |
| §3 | Why does RAII stop at a non-local exit? |
| §4 | What does Go's `defer` actually buy? |
| §5 | What would a trap-cleanup stack look like? (shipped) |
| §6 | What can it clean, and what can it not? |
| §7 | What was broken before revision 3? |
| §8 | What are the alternatives, and which are cures vs workarounds? |
| §9 | How do we keep `longjmp` but confine it? (not taken) |
| §10 | Which option can the three backends support, and what must be refactored? |
| §11 | How does this interact with `HostBridge`? |
| §12 | What shipped, what remains, and how the per-agent state is owned? |
| §13 | What proves it? |
| §14 | What is still open? |
| §15 | References |

## 2. The trap contract we have

`WasmBase/WasmAgent.hpp` keeps the pending trap in the current execution
state's `TrapSlot`:

```cpp
inline constexpr std::size_t kTrapMessageCapacity = 192;
struct TrapSlot { int pending; int kind; char message[kTrapMessageCapacity]; };
extern "C" TrapSlot* wasm_trap_slot() noexcept;   // this thread's slot
```

The message is copied into the slot rather than referenced, because the source
(the `what()` of an exception in a native import, say) may be gone before the
boundary reads it.

A failure is recorded and control returns:

```cpp
WASM::reportTrap(WASM::TrapKind::OutOfBoundsMemoryAccess);
return;                     // or nullptr / 0 — a benign value
```

Generated code gets the same behaviour through `wasm_trap_report(int, const
char*)`, which the backend calls from its trap path before branching to the
function's error epilogue (return zeroes of the result types). Four properties
make the model work:

- **First trap wins.** `reportTrap` ignores a second report while one is
  pending, so a failure during cleanup cannot overwrite the original reason.
- **Thread-local.** The slot is resolved once per function; `wasm_stack_check`,
  `reportTrap` and the boundary all see the same slot on the executing thread.
- **No unwind table needed.** Nothing non-local happens across a generated
  frame: helpers return, generated functions return, and only the final C++
  boundary throws.
- **The boundary checks once.** `WASM::checkTrap()` is `[[gnu::noinline]]`; it
  takes the trap and throws `WASM::Trap` (a copied, fixed-buffer message) when
  `wasmCallDepth() == 0`, and leaves the slot pending when a host import is
  re-entering Wasm.

```cpp
try {
    auto value = WASM::callCallable<int32_t>(*exported, 42);
} catch (const WASM::Trap& trap) {
    std::fprintf(stderr, "trap (%s): %s\n", WASM::trapKindName(trap.kind()),
                 trap.message());
}
```

That is the whole cleanup machinery: ordinary destructors on the way out, plus
the optional `defer` stack for external state. §3 explains why the previous
`setjmp`/`longjmp` model could not offer this.

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

Under the confinement option (§9) the last row narrows rather than disappears:
generated code, the audited helpers and the engine-provided import thunks may
still be skipped, and nothing else may. Under the status option (§8.5) the row
becomes "returns normally, destructors run", and the POD-only rule stops being a
statement about C++ lifetime and becomes a property of an in-process calling
convention.

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
| `panic` | `WASM::trap()` | a trap is a pending status plus a boundary `WASM::Trap`, not a typed Go panic |
| `recover` | the boundary catching `WASM::Trap` | no resumption; the embedder receives the trap kind/message |
| defers run on normal return | explicit `dismiss(token)` before return | requires discipline; a stale record would run on a later trap |
| defers run during panic unwinding | `trap()` runs records down to the active scope's snapshot | runs before `longjmp`, while the native frames are still physically active |

The last row is also the safety boundary: the thunks run while the native
frames are still alive (they unwind normally, they are not jumped over), so a
thunk may use pointers to state owned by those frames. It is still not a
substitute for C++ lifetime rules: a thunk runs *during* the trap path, before
the frame's local destructors.

## 5. Candidate design: a trap-cleanup stack

**Shipped (TRAP-4).** `WASM::defer(fn, arg)` registers a record and returns a
`TrapDeferToken`; `WASM::dismissDeferred(token)` removes it on the normal path;
`reportTrap()` drains the remaining records most-recent-first. The backing store
is `Elv::Util::DeferStack` (`extern/MhLib/Elvavena/Util/ElvDeferStack.hpp`), a
fixed-capacity, non-allocating LIFO. It is an escape hatch for state no object
owns, not a replacement for RAII.

The sketch below is the design as originally written. The shipped API keeps the
record/thunk vocabulary but is a free function pair, not a method on a scope
object — there is no `TrapScope` any more — and `defer()` returns a token
instead of taking a dismiss call by name.

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

1. set the slot's `kind`/`message` (`TrapSlot`);
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

### 5.4 Nesting and execution state

The cleanup stack lives in the execution-state object (`ExecutionState` in
`WasmAgent.hpp`), not in a C++ object per "scope". One state belongs to one
agent (or, later, one continuation context); nesting is a saved index.

The shipped shape is:

- the records and the top index live in `ExecutionState` (`WasmAgent.hpp`);
- `reportTrap()` drains the remaining records most-recent-first; the drain runs
  **before** control leaves the reporting frame, so the thunks operate on live
  state;
- a nested boundary (`callCallable` inside an import, or a nested
  `NativeImportScope`) records the depth on entry and drains only back to it, so
  an inner trap does not release the outer call's resources.

A host thread's default agent is created lazily; `AgentScope` installs another
agent for a call, and a continuation runtime will install its own execution
state for each suspended context. The state no longer depends on the host
thread, which is what TRAP-6 needed.

### 5.5 Cleanup-thunk contract

Every cleanup thunk must be:

- `noexcept`: an exception escaping a cleanup callback during trap handling has
  no safe propagation path through the generated frames;
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
| per-call host-reference arena | yes, if the arena is owned outside the skipped frame and the thunk only drops roots | aligns with `HostBridge` per-instance ownership; see §11 |
| OS file descriptor / mapping / socket | yes | cleanup is a syscall/handle operation, not a C++ destructor |
| `std::unique_ptr` local in a skipped frame | **no** | its destructor is skipped; UB is already in play |
| `std::string` / `std::vector` / `std::function` local in a skipped frame | **no** | same non-trivial-destructor problem |
| `std::lock_guard` / `std::unique_lock` / `std::scope_exit` in a skipped frame | **no** | the cleanup object's own destructor is the mechanism, and it is skipped |
| guest-side Wasm lock or semaphore | **not by this mechanism** | the engine does not track guest lock ownership; this needs a module/engine protocol |
| instance-wide host roots | not enough on its own | `HostBridge::instanceDestroying` and a per-instance arena remain the lifetime backstop |

A defer thunk fixes release on the trap path; it does not make it safe to hold a
non-recursive host mutex across an export whose import re-enters it. That remains
a lock-ordering problem, not a lifetime problem.

Two further consequences follow from the table, and they shape the rest of this
note:

- **A defer stack is a workaround, not a cure.** It bounds the damage of the
  current model for external state; it cannot restore C++ lifetime rules in a
  frame that is skipped.
- **The rows that say "no" are exactly the engine's own problem.** The string
  and array helpers in the backend hold `std::vector` locals across a trap
  point (§7), so a defer stack does not help them: the local has to stop
  existing, not be released early.

## 7. The gap that motivated revision 3: a trapping start function

**Fixed in revision 3.** A trapping start function now throws `WASM::Trap` out of
`instantiate`, so the `std::unique_ptr<ModuleInstance>` is destroyed by C++
unwinding; `a trapping start function unwinds and destroys the partial
instance` in `Test/main.cpp` asserts `instanceDestroying` ran. The section is
kept as the record of why the old model could not fix it.

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
shape is a normal C++ boundary around the start call:

1. do not `longjmp` across the frame that owns the instance;
2. catch the trap in a nested `TrapScope` at a point where the instance holder
   is still a valid C++ local;
3. snapshot the trap kind and message **before** destroying anything, because
   teardown (`~ModuleInstance`, `instanceDestroying`, adapter callbacks) may
   itself record or clear the current execution state's trap state;
4. destroy/reset the instance normally (which invokes
   `HostBridge::instanceDestroying`);
5. re-raise the snapshotted trap to the outer scope, or return a controlled
   instantiation failure.

The holder itself must remain trivially destructible or live in a frame that is
not skipped. A `std::unique_ptr` in the same frame as the nested `setjmp` is
fine only if it is constructed before `setjmp()` and not modified between
`setjmp()` and the jump; that is a constraint on how the fix is written, not a
free pass for arbitrary non-POD locals.

The same gap exists inside the engine's own helpers. The harness cannot see it
because no test asserts on host-visible cleanup, but the frames are skippable
today and the destructors are skipped today:

| Helper kind | Example | Non-POD state live at a trap point |
|---|---|---|
| stringref constructors | `wasm_string_new_utf8`, `wasm_string_new_wtf8`, the `*_array` forms | `std::vector<uint32_t> codePoints` holding the decoded input |
| stringref views / encoders | `wasm_string_encode_utf8`, `…_wtf8`, the iterator helpers | `std::vector<uint8_t> encoded` |
| segments / tables | `memoryInit`, `tableInit`, `bufferInitFrom*` | POD today, deliberately ([`SAFETY.md`](SAFETY.md) §2.6) |

This start-function gap is independent of the host adapter and is required under
every option in §8 — including the status option, where it becomes an ordinary
`if (trapped) instance.reset();` instead of a nested `setjmp`.

## 8. The option set, and what class each option belongs to

This section replaces the piecemeal "alternatives" lists of the earlier
revisions with one classification, one table, and a short assessment per option.
§10 then asks what each option costs on the three backends.

### 8.1 Cures and workarounds

The mechanisms fall into two classes:

- **Workarounds** keep `longjmp` and give native code an explicit escape hatch
  for *external* state: the defer stack (§5) and the per-import `setjmp`
  boundary (§8.3). Confinement (§9) is a third workaround of a different kind:
  it does not add a cleanup hook, it forbids the trap from crossing frames that
  would need one. None of the three restores C++ lifetime rules; a non-trivial
  local in a skipped frame is still undefined behavior.
- **Cures** replace the non-local exit and therefore make ordinary RAII valid
  again in frames that currently can be skipped: status-return traps (§8.5) and
  C++ exceptions with unwind tables (§8.4).

The distinction matters for planning. A workaround bounds the damage of the
current model; a cure changes the model. TRAP-1 (§7, §12) is neither — it
removes one concrete UB site under the current model and is required regardless
of which direction is later taken.

| Option | Class | Fixes the start-function gap? | Relaxes the POD-only rule? | Backend work |
|---|---|---|---|---|
| Defer stack (§5) | workaround | no | no | none |
| Per-import `setjmp` (§8.3) | workaround | no | only inside the import body | none |
| Confinement (§9) | workaround | yes, via `callGuarded` (§9.4) | shrinks the rule; does not remove it | a wrapper per import signature, or a value-array ABI (§10.4) |
| Status-return traps (§8.5) | cure | yes, with ordinary C++ | **yes, removes it** | per-backend epilogue + call-site checks |
| C++ exceptions (§8.4) | cure | yes | **yes** | CFI/personality in every backend |
| Signals / SEH (§8.6) | n/a | no | no | platform-specific |

### 8.2 The comparison table

| Option | What it buys | Cost / risk | Verdict |
|---|---|---|---|
| Keep the current POD-only rule | no new runtime mechanism; the existing contract is honest | host locks/arenas still leak or deadlock on a trap; the start-function leak remains; the string/array helpers are already UB (§7) | status quo at the time, not sufficient — **the rule is gone** (TRAP-3) |
| C++ scope guards / `std::scope_exit` | familiar RAII syntax | destructors are exactly what `longjmp` skips; no improvement | does not solve it |
| Go-style fixed-capacity cleanup stack (§5) | explicit trap-time release of host locks, arenas and handles; no backend or ABI change | normal-path dismiss discipline; fixed cap; callbacks must be no-throw/non-reentrant; still cannot make non-trivial locals safe | **shipped** as an escape hatch (TRAP-4): `WASM::defer` |
| Per-import `setjmp` boundary (§8.3) | import-local cleanup of non-trivial locals in the import's own frame | `setjmp` cost on every participating import; re-raise discipline; does not fix engine-internal frames | **narrow workaround**, interchangeable with the defer stack |
| Confinement (§9) | most frames become unskippable, so native import bodies and `instantiate` may use ordinary C++ | import-ABI change or a backend-generated wrapper per import; a closed audited-helper list; a `callGuarded` per re-entry unless §9.7 is used; the rule must be policed in review | **was** the recommended intermediate step (§10.2); **not taken** — §8.5 replaced the mechanism instead |
| Status-return traps (§8.5) | removes the POD-only rule; ordinary RAII everywhere, including engine frames; no non-local exit at all | helper ABI conversion, an error epilogue per function, a check per call/helper site; a per-call-site branch (predict-not-taken) and extra code size | **shipped** as the pending-slot variant (TRAP-3) |
| C++ exceptions with unwind tables (§8.4) | real C++ unwinding and RAII | no backend emits CFI; needs a documented ABI, a personality routine, `.eh_frame` registration and platform-specific work per backend; harder to debug across generated frames | not recommended |
| Signals / SEH (§8.6) | catches hardware faults without instrumenting the instruction | Wasm traps here are explicit software checks, not faults; handlers are async-signal-unsafe and cannot run destructors either | rejected |
| Engine-owned lock/resource registry | can release or poison host and guest locks after an agent dies; supports cancellation | needs per-agent ownership tracking, poisoning semantics, and a policy for partially executed critical sections | separate design item, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1 |
| Reuse the Wasm exception machinery for traps | one unwinding design; `try_table`/tags already exist | a trap must be *uncatchable* by `try_table`, so the two paths stay distinct in codegen; still needs the status plumbing | a subset of §8.5, not a separate option |

Footnote to the table: workarounds (defer stack, per-import `setjmp`,
confinement) bound the damage of the current `longjmp` model; cures (status
returns, C++ exceptions) change the model. TRAP-1 is independent of both and is
required under the current model either way.

### 8.3 Per-import `setjmp` boundary (narrow workaround)

**Not taken.** The `nativeImport` trampoline (§9.2/TRAP-5) is what shipped:
a `try` around the body, no `setjmp`, and the same "the import's own frame is
ordinary C++" property.

Each native import that wants cleanup arms its own `TrapScope` around the call
back into Wasm, catches the trap locally, releases its own non-trivial locals
through ordinary C++ control flow, and re-raises.

What it buys:

- an import can hold non-trivial locals in *its own* frame, because it catches
  the trap before the `longjmp` reaches it;
- per-import cleanup does not require the defer stack or any new engine API.

What it costs:

- every participating import pays a `setjmp` — tens of cycles on typical ABIs,
  plus a compiler barrier; a real tax on hot imports;
- the re-raise must not be caught by the same scope again; `setjmp` clobber
  rules and the indeterminate-value rule of §5.4 are easy to get wrong;
- it does not address engine-internal frames such as the start-function path
  (§7), so TRAP-1 remains required;
- it is still `longjmp`, with a narrower blast radius.

Assessment: interchangeable with the defer stack for the "release external state
on the trap path" use case. The trade is `setjmp` cost and re-raise discipline
against the defer stack's explicit-dismiss discipline and fixed capacity. Neither
is obviously better; the choice belongs with the first concrete import that needs
it (§12, TRAP-5). §9.7 describes a variant that keeps the import body's frame
alive without a per-import `setjmp`.

### 8.4 C++ exceptions with unwind tables

**Rejected** on every available backend (§10.1: no backend emits CFI, and
LibJIT offers no hook to attach it).

Traps become a C++ `throw` of a dedicated exception type, and every generated
frame carries valid unwind information so the platform unwinder can step through
it (`.eh_frame`/DWARF CFI on System V, `.pdata`/`.xdata` on Windows).

What it buys:

- real destructor execution in every frame the unwinder walks, including
  generated ones;
- ordinary `try`/`catch` at the embedder boundary and inside native imports;
- no explicit cleanup protocol and no POD-only rule.

What it costs:

- **unwind metadata in every generated frame.** None of the three engines emits
  it (§10.1), so it has to be produced by the backend — a CFI generator, the
  per-PC stack-state tracking to feed it, `.eh_frame` registration with the
  runtime, and a personality routine that understands the frame layout;
- a documented ABI for exceptions crossing generated frames, and a policy for
  native imports and host-language adapters on top of it;
- materially worse debuggability: a mistake in the CFI data aborts the process
  at the throw point instead of failing a test;
- a second unwinding mechanism next to any future Wasm exception-handling
  design, which must be resolved in one go rather than two.

Assessment: the semantics are the best of any option and the portability is the
worst. It should be re-evaluated only together with Wasm exception handling and
only on a backend where the CFI work is tractable (§10.2). It is not a candidate
for fixing the trap path on its own.

### 8.5 Status-return traps (the cure)

**Taken — but in the second shape.** TRAP-3 shipped the pending-slot variant
(the one the engine already used for Wasm exceptions); the first variant below
(status in the return value) was rejected on cost, because it changes every
signature including imports and the tail-call trampoline.

Every call into a helper or into generated code reports a trap through a status,
and the code that made the call branches to a shared error epilogue instead of
being jumped over. Two shapes are possible, and the difference matters:

- **Status in the return value.** Every generated function and every helper
  returns `TrapStatus`/`ImportResult`. This is the shape the earlier revisions
  described and costed: it changes `Callable::fnPtr`, every native import
  signature, the multi-value result-buffer convention and the tail-call
  trampoline slots.
- **Status in a pending slot.** A helper that traps records `{kind, message}`
  in thread-local state and returns a benign value; the call site checks the
  slot immediately afterwards and branches to the epilogue. The function
  signatures — including `Callable::fnPtr` and the native import ABI — stay
  exactly as they are.

The engine already implements the second shape for Wasm exceptions: `throw` sets
the current execution state's pending-exception slot (`wasm_exn_slot()`), returns zeroes from the current function
(`emitReturnZeroes`), and every call site runs `emitPendingExceptionCheck`. The
feasibility study (§10.3) argues that this makes the pending-slot variant the
realistic one, and that the cost estimates in earlier revisions were too
pessimistic because they assumed the first shape.

What it buys (either shape):

- no non-local exit, so C++ destructors run on every path, including native
  imports and engine-internal frames;
- native imports may hold ordinary `std::lock_guard`, `std::unique_ptr`,
  `std::string` and other non-trivial locals without qualification;
- the defer stack, the POD-only rule and `TrapScope` all become unnecessary for
  cleanup purposes;
- interaction with a future Wasm exception-handling proposal is a single design
  problem rather than two coexisting unwinding mechanisms;
- an error epilogue is cheap in this backend design: the Wasm operand stack
  lives in frame locals, so the epilogue does not have to unwind a machine
  operand stack, only return the zero of each result type.

What it costs (pending-slot shape):

- every helper that can trap must stop calling `[[noreturn]] trap()` and start
  reporting through the slot, then return a value of the declared type;
- every call site that can reach such a helper needs a check-and-branch;
- the trap must never be routed through the Wasm `try_table` exception path,
  and the propagation check must be trap-first;
- a check on the hot path (predict-not-taken in practice, but not free) and
  additional code size for the epilogues;
- `WASM::trap` stops being `[[noreturn]]` for helper use. That is an
  improvement for the helper (a missing return becomes a compile error) and a
  new hazard for the call site (a missing check silently swallows the trap), so
  it needs the tripwire of §9.6 until the conversion is complete.

Assessment: this is the design that would have been chosen had the trap path
been specified with C++ lifetime rules as a constraint. It is not a patch, but
on LibJIT it is a transformation of an existing mechanism rather than a new one
(§10.4). It should be tracked as the principled long-term direction and gated on
a concrete trigger (§10.5).

### 8.6 Signals and structured exception handling (rejected)

A hardware-fault model (guard pages plus `SIGSEGV`/`SIGFPE`, translated into a
throw through `_set_se_translator` or a `sigaction` handler) is how some engines
avoid an explicit bounds check. It does not apply here:

- the engine's traps are **software checks** — bounds tests, null tests, divide
  by zero, `unreachable` — not faults, so there is nothing for a handler to
  catch without deliberately removing the checks;
- a signal handler cannot run C++ destructors safely and cannot allocate, so it
  does not solve the problem this note is about;
- the platform surfaces differ (SEH vs signals vs `sigaltstack`), which
  contradicts the layering rule that the core states a contract rather than a
  platform ([`LAYERING.md`](LAYERING.md) §2);
- the stack-exhaustion path already converts the one genuine fault into a
  checked trap before it happens ([`SAFETY.md`](SAFETY.md) §2.8), which is the
  property we would want for every other trap too.

The one place the idea is legitimate — guard-page bounds checks for linear
memory, as Wasmtime does — would be a *performance* change to the memory path,
not a trap-cleanup mechanism, and it would still need the status/propagation
work of §8.5 to leave generated code.

### 8.7 Production precedent

- **Wasmtime** used `setjmp`/`longjmp` for traps since its inception and replaced
  them in September 2025 (PR #11592) with a trampoline-based `try_call` built on
  Cranelift's exception-handling machinery. The stated reasons map onto this
  note almost one-to-one: `longjmp` skips destructors in the frames it crosses,
  Rust cannot model a function that returns twice, and a second unwinding
  mechanism coexisting with the Wasm exception proposal is untenable. The
  property they kept is O(1) traps: the handler frame information lives in the
  host-to-Wasm trampolines rather than in a general unwind.
- **SpiderMonkey** has long used specialised entry trampolines plus a throw stub
  that walks a maintained frame structure (or plants a sentinel) and returns to
  the trampoline, which then reports an exceptional result to the host — again
  no general-purpose `longjmp` over arbitrary frames.

Both are the "trampoline boundary + status" shape of §9.7: one well-defined
boundary that receives the failure, instead of a jump that crosses everything.

## 9. Confinement: keep `longjmp`, shrink the skippable set

**Not taken.** Revision 3 removed `longjmp` instead of confining it, so this
section is the record of the intermediate option and of why §9.7 turned out to be
a stepping stone towards the status model rather than its endpoint.

Sections 5–6 try to make a skipped frame *survivable* with a cleanup stack. The
confinement option instead makes most frames *unskippable*: `longjmp` is kept,
but the set of frames it may cross is shrunk to something small and auditable.

### 9.1 Rule R2 and the frame table

> **Rule R2.** A trap may skip only generated frames, engine runtime helpers on
> the audited list (§9.5) and engine-provided import thunks. No other frame may
> sit between a trap point and its `TrapScope`.

Everything outside that set may use ordinary C++: `std::lock_guard`,
`std::unique_ptr`, `std::string`, exceptions. That includes native import
bodies, `ModuleInstantiator::instantiate`, and the embedder.

| Frame | May be skipped by a trap? | Non-trivial locals allowed? |
|---|---|---|
| Generated Wasm code | yes | no (nothing to destroy) |
| Audited runtime helper (§9.5) | yes | **no**, POD-only as in [`SAFETY.md`](SAFETY.md) §2.6 |
| Import thunk (engine-provided) | yes | **no**, POD-only |
| Native import **body** | **never** | **yes** |
| `callGuarded` (§9.3) | never; it is the jump target | only trivially destructible locals between `setjmp` and the jump |
| `instantiate`, embedder code | never | yes |

The property that makes this work is ordering: an import body *returns* a status
and is gone before the trap is raised, so its destructors have already run when
`longjmp` happens.

### 9.2 Import contract

**Shipped form (revision 3).** The wrapper is `WASM::nativeImport<&fn>()`
(`WasmBase/WasmNativeImport.hpp`): a per-function template trampoline that
catches `WASM::Trap` / `std::bad_alloc` / `std::exception` / `...`, reports the
equivalent pending trap, and returns a benign value. It keeps the import’s exact
Wasm signature, so no `ImportResult` type and no engine-provided thunk are
needed for a free function whose per-instance state arrives through
`VMContext::hostData`. A *capturing* callable still needs the backend-generated
wrapper described below, which is the only part of TRAP-5 left open.

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

The message is copied inline on purpose. A `std::string` or a `const char*` into
the body's own storage would dangle by the time the thunk reads it.

The engine supplies the thunk between generated code and the body:

```cpp
// POD-only frame. Skippable by design.
void importThunk(VMContext* ctx, const Value* args, Value* results) {
    ImportResult r = runImportBody(ctx, args, results);   // body frame returns first
    if (r.trapped)
        WASM::trap(r.trap.kind, r.trap.message);          // copies into TrapState, then longjmps
}
```

`ImportResult` is trivially destructible, so the thunk frame is legal to skip.
`WASM::trap` copies the message into thread-local `TrapState` before jumping, so
no pointer into a dead frame survives.

Body obligations:

- must not call `WASM::trap`, `longjmp`, or throw across the thunk. An exception
  escaping a body is caught by `runImportBody` and converted to a trap (or
  aborts, see §9.8);
- may hold any C++ resource, including across a re-entrant call into Wasm
  (§9.3);
- reports a failure only by returning `trapped = true`.

### 9.3 Scoped re-entry: `callGuarded`

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
    CallResult r{false, {}};         // constructed after the jump: not clobbered
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

### 9.4 `instantiate` and the start function

The start function is the same pattern with the instantiator as the guarded
caller:

```cpp
auto instance = std::make_unique<ModuleInstance>(module, resolver, hostBridge);
// ... type translation, declaration, compilation, segments ...
if (module.hasStartFunction) {
    CallResult r = callGuarded(startCallable(*instance), nullptr, nullptr);
    if (!r.ok) {
        instance.reset();            // ~ModuleInstance -> HostBridge::instanceDestroying
        // contract decision (TRAP-1): re-raise via WASM::trap, or throw a C++ instantiation error
    }
}
return instance;
```

This supersedes the "nested catch boundary" wording in §7 and removes the need
for a bespoke nested `TrapScope` inside `instantiate`. The open contract
question (re-raise vs. convert to an exception) is unchanged and belongs to
TRAP-1.

### 9.5 The audited helper list

Rule R2 is only as strong as the list of things a trap may cross. The list is
closed and maintained in one place (proposed: a table in
[`SAFETY.md`](SAFETY.md) §2.6, mirrored by one namespace such as `WASM::rt`):

- helpers called directly from generated code (`memory.grow`, table operations,
  reference-count barriers `refClone`/`refStore`/`refDrop`, bounds-failure
  paths, the SIMD helpers, and similar);
- the engine-provided import thunks of §9.2.

Each entry is POD-only. A helper that needs non-trivial locals is not allowed to
grow them; it must instead return a status that the generated code branches on,
or be split so the non-trivial part runs in an unskippable frame. The string and
array helpers of §7 are exactly this case: their `std::vector` work has to move
into a helper that *returns*, with the trap raised afterwards from a POD thunk.

This is a review-enforced list. There is no reliable compiler check for "no
non-trivial locals in this function", so §9.6 adds a runtime tripwire and §10.4
lists the specific helpers that fail the rule today.

### 9.6 Debug tripwire

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
a review item. The same mechanism generalizes to the status option of §8.5: a
"pending trap set but returned to a non-checking frame" state is the analogue of
`inImportBody`, and it is worth having one flag shape for both.

### 9.7 Boundary rethrow: keeping `TrapScope` without skipping frames

The per-import `setjmp` of §8.3 and the `callGuarded` of §9.3 both pay a
`setjmp` per re-entry. There is a cheaper shape that combines confinement with
the pending-failure protocol the engine already uses (§8.5), and it is the shape
the recommendation in §10.5 lands on:

1. Generated code and audited helpers never call `longjmp`. A trap records
   `{kind, message}` in the current execution state's trap state and *returns* — the
   generated function returns the zero of each result type, exactly as
   `emitReturnZeroes` already does for a pending Wasm exception.
2. Every call site checks the pending slot and branches to the function's error
   epilogue, which returns the zeroes. Callers therefore unwind by returning,
   through frames that are all C++-visible.
3. The **host-to-Wasm boundary** — the per-function driver (or a dedicated
   entry trampoline, for a backend that has one) — is the only place that
   converts a pending trap back into a non-local exit. It calls a single,
   `[[gnu::noinline]]`, POD-only bridge that reads the slot and `longjmp`s to the
   armed `TrapScope`. Only the bridge frame is skipped, and it has nothing to
   destroy.
4. When the boundary is entered from an import body (a re-entry), the bridge
   does **not** jump: it leaves the trap pending and returns, so the import body
   observes the status and can release its locals normally. This is the same
   distinction the `inImportBody` flag makes in §9.6.

What this buys over §9.3: no `setjmp` per re-entry, no `ImportResult` signature
change for import bodies (they check the slot instead), and the embedder-facing
`TrapScope` + `setjmp` pattern is untouched — the jump still happens, but only
across frames that are generated or on the audited list.

What it costs: the boundary must be a place the engine controls (it is: the
per-function driver is generated by the backend), the pending slot must be
per-agent and trap-first relative to the Wasm exception slot, and the
"am I inside an import body?" test must be exact, because a wrong answer either
jumps over an import body (the bug we are removing) or fails to report a trap to
the embedder.

Open questions are recorded in §14. This is a candidate, not an implemented
design; the name is deliberately descriptive rather than final.

### 9.8 Costs and risks

- **API break.** Existing native imports that call `WASM::trap()` must return
  `ImportResult` instead (or, under §9.7, report through the pending slot).
  Out-of-tree embedders break at compile time, which is the desired failure mode.
- **Leak through raw calls.** The guarantee holds only if every path from an
  import body into generated code goes through the guarded or checking entry
  point. Do not expose a raw call API to import authors.
- **Exceptions escaping a body.** `runImportBody` must either translate them to
  a trap or `std::abort`. Letting one propagate into the thunk and generated
  frames is undefined (no unwind tables).
- **`setjmp` cost per re-entry** (only for the §9.3 shape). One `setjmp` per
  host-to-Wasm re-entry. Check whether the implementation saves the signal mask
  (`_setjmp` or `sigsetjmp(buf, 0)` avoid it). §9.7 removes this cost.
- **Review burden.** The audited list is enforced by review and a debug
  tripwire, not by the compiler; the string/array helpers show that the tree
  already drifted.
- **Unchanged problems.** Guest locks (§14) and lock-ordering across re-entrant
  imports are not addressed by confinement.

### 9.9 What confinement changes in this note

| Section | Effect |
|---|---|
| §3 table, "Wasm `longjmp` trap" row | The "native path must be POD-only" clause narrows to "generated code, audited helpers and thunks". |
| §5 defer stack | Demoted. The main use cases (host mutex or per-call arena held across a trapping call) now live in an import body that cannot be skipped. Remaining use: resources held by an audited helper across a trap, which should be rare. |
| §6 table | Rows for `std::lock_guard` and `std::unique_ptr` become "yes, in an import body"; they stay "no" in a skipped frame. |
| §7 | Fixed by §9.4 using `callGuarded` (or, better, by §9.7). |
| §8.2, "per-import `setjmp`" row | Cost objection no longer applies to imports that merely *call* Wasm: they do not pay a `setjmp`. An import that *re-enters* Wasm pays one per re-entry under §9.3, and none under §9.7. |
| §11 per-call arena | An adapter's per-call arena can be an ordinary RAII object in the import body. |
| §14 "Normal-path ergonomics" | Largely moot: no `dismiss` discipline on the common path. |

## 10. Feasibility against the three backends

This is the part a design decision actually turns on. The option set of §8 is
not equally available on the three code generators this project intends to run:
one is implemented, two are scaffolds. The question per option is not "is it
possible in principle" but "what does it cost on each engine, and which engine
blocks it outright".

Sources for this section: the installed GNU LibJIT headers and library
(`/usr/local/include/jit`, `/usr/local/lib64/libjit.so.0.0.1`), the SLJIT public
header and README, the AsmJit API reference and issue tracker, and the code in
`LibJit/`. `Asmjit/Asmjit.pro` and `Sljit/Sljit.pro` contain no sources, so
"SLJIT/AsmJit cost" means greenfield work, not porting.

> **Status:** this is the *pre-change* study (revision 2) that chose the model.
> §10.1–§10.4 describe the tree **as it was before TRAP-3** — `setjmp`/`longjmp`,
> `TrapScope`, the POD-only rule, process-global trap state — and are kept as the
> record of why the choice was made; §10.5 is the resulting order, with its
> per-step outcome. Two names below are the pre-change ones: `TrapState` shipped
> as `TrapSlot`, and `emitAbort` does not exist (the second emitter helper is
> `emitTrapIfNull`); §12 has the shipped inventory.

### 10.1 What each engine actually offers

| Capability | GNU LibJIT (installed, primary) | SLJIT (scaffold) | AsmJit (scaffold) |
|---|---|---|---|
| Value model | typed `jit_value_t`, function-local slots, structs, signatures | none — a portable register machine; the caller owns the layout | assembler/IR; `Compiler` adds register allocation, `FuncNode` describes a function |
| Branch/label control flow for an error epilogue | `jit_insn_branch`, `jit_insn_branch_if`, labels | `sljit_emit_jump`, `sljit_emit_cmp`, labels (a jump may even target another function in the same context) | labels plus `jmp`/`jcc` directly |
| Call with an arbitrary signature | `jit_insn_call_native` / `jit_insn_call_indirect` + `jit_type_create_signature` | `sljit_emit_call`/`sljit_emit_icall` with an explicit convention | `invoke` with `FuncSignature`/`FuncDetail` |
| Function-local storage for a status word | `jit_value_create` locals | an explicit offset into the local area `sljit_emit_enter` reserves | stack slots via `FuncFrame` (`local_stack_size`) |
| **Unwind info / CFI / `.eh_frame` emission** | **none** — no API, no CFI directives, no `.eh_frame` | **none** — no API | **none** — no API; the feature request (asmjit#313) was closed by the maintainer in September 2025 |
| Platform EH integration (SEH, personality, `.pdata`) | none | none | none |
| Own exception facility | `jit_insn_throw` / `jit_insn_start_catcher` / `jit_insn_start_finally`, internally a private `setjmp` chain (`_jit_unwind_push_setjmp` / `_jit_unwind_pop_setjmp`) | none | none |
| Frame introspection for walking the stack | `jit-unwind.h` (`jit_unwind_init`/`next`/`get_pc`/`get_function`/`get_offset`/`jump`) — LibJIT's own walk, not usable by the C++ personality routine | none | none |
| Per-function entry trampoline | already generated by this backend (the tail-call driver) | must be emitted by the backend (`sljit_emit_enter` + a custom prolog) | must be written by hand or generated by `Compiler` |
| State in this tree | the only executing backend (`LibJit/`) | scaffold (`Sljit/Sljit.pro`, no sources) | scaffold (`Asmjit/Asmjit.pro`, no sources) |

Three consequences:

1. **C++ exceptions are out on LibJIT and SLJIT, and bespoke on AsmJit.** Nobody
   emits CFI. LibJIT offers no per-instruction hook at which CFI could be
   attached, and its own EH is a `setjmp` chain that skips C++ destructors in
   exactly the same way the current trap path does. AsmJit is a real assembler,
   so a backend *could* emit `.cfi_*`-equivalent bytes into `.eh_frame` and
   register it — but that is an unwinder project, not a backend feature, and the
   maintainer has declined to own it.
2. **The status/pending-slot option needs nothing a backend lacks.** All three
   can emit a compare-and-branch, all three can call a helper that returns a
   value, and all three can keep a status word in a frame slot or in
   thread-local storage. It is the only *cure* available on every engine.
3. **The workaround options (defer stack, per-import `setjmp`) need no backend
   support at all**, because they live entirely in the trap state and in C++
   frames. Their cost is discipline and, for `setjmp`, hot-path overhead — not
   backend work.

### 10.2 Option × backend matrix

Effort scale: **S** — one file plus tests; **M** — one subsystem plus tests;
**L** — a design of its own spanning the core and one or more backends;
**—** — not reachable.

| Option | LibJIT | SLJIT | AsmJit | Blocking fact |
|---|---|---|---|---|
| Defer stack (§5) | S | S | S | none; the backend is untouched |
| Per-import `setjmp` (§8.3) | S | S | S | none, but each re-entering import pays a `setjmp` |
| Confinement, thunk shape (§9.2–§9.4) | M | M | M | the per-signature thunk has to be generated by the backend or reached through a value-array ABI (§10.4) |
| Confinement, boundary-rethrow shape (§9.7) | M | M | M | needs a call-site check after helper/import calls — which is the same work as the pending-trap status |
| Pending-trap status (§8.5) | M | M–L | M–L | LibJIT already has the machinery; the scaffolds do not, but nothing blocks it |
| C++ exceptions (§8.4) | — | — | L | no engine emits CFI; on LibJIT there is no hook to add it |
| Signals / SEH (§8.6) | — | — | — | traps here are software checks, not faults |

The qualitative comparison that usually drives the argument:

| Approach | C++ destruction / RAII | Happy-path overhead | Backend complexity | Embedder ergonomics |
|---|---|---|---|---|
| `setjmp`/`longjmp` (the pre-TRAP-3 model) | **broken** (UB for non-POD) | zero | low | poor: manual `defer` or nested scopes |
| Defer stack | none in skipped frames; external state released | zero | none | moderate: register/dismiss discipline |
| Confinement | full, outside the audited set | zero | low–medium: import thunk or call-site check | good: ordinary C++ in import bodies |
| Pending-trap status | **full**, everywhere | one predict-not-taken branch per call/helper site | medium: epilogue + check, reusing existing machinery | good: the boundary throws `WASM::Trap` (§9.7, as shipped) |
| Status in the return value | **full**, everywhere | call-site branch + wider returns | high: ABI, tail calls, multi-value, imports | good, but every import signature changes |
| C++ exceptions | **full**, everywhere | zero on the happy path | very high: CFI, personality, registration, per platform | good, where it works at all |

### 10.3 What the tree already gives us

The estimate changes materially once the existing code is counted, because
LibJIT already implements the propagation half of the pending-slot design for
Wasm exceptions. Concretely, in `LibJit/` and `WasmBase/`:

| Fact | Where | Why it matters |
|---|---|---|
| A pending-failure slot that crosses module and function boundaries | `wasm_exn_slot()` (`WasmExn.cpp`) | the trap status reused the pattern and the TLS discipline |
| Generated functions return zeroes when a failure is pending | `emitReturnZeroes()` | a trap epilogue already exists in concept: "return the zero of each result type" |
| Every call site already checks a pending slot | `emitPendingExceptionCheck()` after `call`, imported `call`, internal `call`, `call_indirect` | adding a trap check is one more comparison at the same four emission points, plus the helper calls |
| All generated-code traps funnel through two emitter helpers | `emitTrap()` / `emitTrapIfNull()` (`LibjitOpcodeDispatcher.cpp`) | the *failure target* changed; the 21 inline checks that call them did not |
| The Wasm operand stack lives in frame locals, not on a machine stack | `newV128Slot`, `pushValue`/`popValue`, `ControlBlock` slots | the error epilogue has nothing to unwind — no stack-depth bookkeeping, no `alloca` discipline |
| A per-function entry trampoline already exists | the tail-call driver in `LibjitModuleCompiler.cpp` | the §9.7 boundary is a place the engine already generates |
| ~49 native-helper call sites (`jit_insn_call_native`), ~21 inline `emitTrap` sites | `LibjitOpcodeDispatcher.cpp` | the size of the mechanical conversion, and it is mechanical |
| ~55 `trap()` calls inside helper frames | `LibjitOpcodeDispatcher.cpp` | each is a candidate for "report + return"; the count is bounded and known |
| Helpers holding non-POD locals at a trap point | the stringref/array helpers (§7) | the POD-only rule was *already* violated; part of the work was required whatever was decided |
| Ten embedder/harness `setjmp` sites **at the time** | `Test/main.cpp`, `Test/WastScript.cpp`, `Test/TestBackend.cpp` | the blast radius of changing the embedder contract — and the reason §9.7 proposed keeping a boundary bridge. They are **zero** today: the boundary throws `WASM::Trap` |
| Two failure mechanisms coexisted in the tree | `longjmp` for traps, pending slot + return for Wasm exceptions | the duplication Wasmtime cited as a reason to move was already here — and is gone: TRAP-3 deleted the `longjmp` half |

The last row is why the change was small rather than speculative: the engine
already implemented the Wasm exception proposal with a *status* model (`throw`
becomes a pending payload plus a normal return) while traps used `longjmp`, so
every argument in §8.7 about dual unwinding mechanisms applied to this tree at
the time, not hypothetically. TRAP-3 unified them on the status model.

### 10.4 The refactoring each option needed

**Defer stack (§5).** `WasmBase/WasmTrap.hpp/.cpp` only: the fixed-capacity
record array and its top in the trap state, `TrapScope` saving depth before
`setjmp` (both now gone), `defer`/`dismiss` (shipped as `WASM::defer` /
`WASM::dismissDeferred`), the drain loop and the re-entrancy guard, plus debug
assertions. No backend file changes, no ABI change, no test-harness change. It
fixes neither §7 nor the string/array helpers.

**Confinement, thunk shape (§9.2–§9.4).** The visible cost is the import ABI; the
hidden cost is *who writes the thunk*. Generated code calls `Callable::fnPtr`
directly, so the "engine-provided thunk" between generated code and the body has
to match each import's exact signature. The engine cannot synthesise such a
wrapper in portable C++; the realistic options are:

- the backend generates one wrapper function per import at instantiation time
  (LibJIT: `jit_function_create` with the import's signature, a
  `jit_insn_call_native` to the body, then `wasm_trap_raise` if it trapped);
- a value-array import ABI — `wasm_call_import(index, ctx, const Value* args,
  Value* results)` — which changes the calling convention documented in
  [`ABI.md`](ABI.md), rewrites every import, and complicates multi-value and
  tail calls;
- `libffi`-style runtime closures, i.e. a new dependency.

So confinement is *not* free of backend work: it either adds a wrapper generator
to every backend or changes the import ABI. The earlier revision's "no JIT/ABI
change" claim holds for the defer stack, not for confinement.

**Confinement, boundary-rethrow shape (§9.7).** The thunk disappears. A native
import that traps records the trap and returns; the *generated code* checks the
slot right after the call — at the same four sites that already check the
pending exception — and branches to its epilogue. The import body keeps its
existing signature, so no ABI change and no wrapper generator. What remains is
the state machine: pending trap slot, call-site checks, boundary bridge.

**Pending-trap status (§8.5, the §9.7 shape).** The concrete work list. This is
the list TRAP-3 executed, so it reads as the plan it was; the two deviations are
noted in the rows they touch:

| File | Change |
|---|---|
| `WasmBase/WasmTrap.hpp/.cpp` | add a pending trap to the slot (shipped as `TrapSlot`); add `reportTrap(kind, detail)` (returns) and `trapPending()`/`clearTrapPending()`; keep `trap()` `[[noreturn]]` only for the boundary path, implemented as `reportTrap()` + the throw. **Deviation:** `TrapScope` was deleted rather than kept — the boundary throws `WASM::Trap` |
| `WasmBase/WasmModuleInstance.cpp` | convert the ~25 `trap(...)` sites in `memoryInit`/`tableInit`/`bufferInitFrom*`/GC allocation to report-and-return (each function gains a status); the start-function call returns a status so `instantiate` can `instance.reset()` |
| `WasmBase/WasmString.cpp`, `WasmExn.cpp`, `wasm_atomic.cpp` | same conversion for the remaining helper trap sites |
| `LibJit/LibjitOpcodeDispatcher.cpp` | `emitTrap`/`emitTrapIfNull` become "record the trap, branch to this function's error epilogue"; add `emitPendingTrapCheck()` after helper calls and at the four existing `emitPendingExceptionCheck()` sites; drop the non-returning raise helper (`JIT_CALL_NORETURN`); make exception propagation trap-first so `try_table` never catches a trap. **All four shipped.** |
| `LibJit/LibjitModuleCompiler.cpp` | the driver checks the pending slot after the callee returns and returns immediately; the start function is called through the status-returning path |
| `Test/` | unchanged if `TrapScope` keeps its `setjmp` (boundary rethrow); otherwise ~10 sites move to a status check |
| `docs/SAFETY.md` §2.6, `docs/THREADSAFETY.md` §10.1, `docs/ABI.md` | record the new rule once the model is chosen; until then §2.6 stays in force |

Two properties make this a transformation rather than a rewrite on LibJIT:

- the failure *checks* already exist (four call sites, one pending slot) and the
  failure *branches* already exist (21 inline `emitTrap` sites, whose control
  flow does not change — only the helper they branch to);
- the epilogue already exists as a concept (`emitReturnZeroes`), and the operand
  stack is in frame locals, so no machine-stack unwinding is involved.

**C++ exceptions (§8.4).** A new unwind-info generator in whichever backend owns
the model, an `.eh_frame`/`.pdata` registration path, a personality routine, and
a documented ABI — plus the decision about how it coexists with the Wasm
exception machinery already in the tree. LibJIT cannot host the generator at all.

### 10.5 Recommendation and gating conditions

**Revision 2 revises the earlier recommendation.** The earlier revisions
recommended "TRAP-1 now, then a defer stack only if needed, and do not replace
`longjmp` for this work". The feasibility study above shows that position is too
conservative:

- the POD-only rule is already violated by the string/array helpers, so a
  restructuring of those helpers is required whatever is decided;
- the engine already runs a status model for Wasm exceptions, so a trap status
  would not add a second mechanism next to a `longjmp` model — it would make the
  two mechanisms one;
- no backend can host a C++-exception model, and the defer stack cannot fix the
  engine-internal frames;
- the LibJIT codegen change is two emitter helpers, four call-site checks, one
  driver check and one epilogue — not a redesign.

Recommended order (**followed in revisions 3–4** — TRAP-1…TRAP-7 are
shipped; TRAP-6 is implemented, see §12.1):

1. **TRAP-1** — the start-function boundary. Required under every option.
2. **TRAP-2** — the trap-path audit: publish the audited list and split the
   helpers that hold non-trivial locals across a trap point. Required *today*:
   those frames are undefined behavior as written.
3. **TRAP-3** — the pending-trap status with boundary rethrow (§9.7): trap
   state, call-site checks, the driver check and the epilogue, with `TrapScope`
   unchanged. This is the cure, and it is gated on TRAP-2 (which is most of its
   helper-side work).
4. **TRAP-4** — the defer stack, only if a TRAP-2 audited helper needs to
   release *external* state across a trap. Nothing in the tree does today.
5. **TRAP-5** — the import contract of §9.2/§9.3 (shipped as the
   `nativeImport` trampoline, §9.2).
6. **TRAP-6** — per-agent trap state, **implemented** (`ExecutionState` owned by
   `Agent`; §12.1, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1). The multi-agent
   execution model remains deferred.

When to revisit each option:

- **Status-return traps:** revisit if any of (a) an engine-internal frame other
  than the start function must hold a non-trivial local across a trap-capable
  call; (b) a Wasm exception-handling design is adopted, so a single failure
  mechanism is required; (c) the defer stack's discipline proves to be a
  recurring source of review bugs; (d) a second backend (SLJIT/AsmJit) starts
  emitting traps, so the model is chosen once instead of twice. Conditions (a)
  and (b) were **already true** in this tree, which is why TRAP-3 was adopted in
  revision 3 rather than merely recorded.
- **Per-import `setjmp`:** at TRAP-5 time, compared against the defer stack on
  the first real import, not chosen abstractly. §9.7 removes its main drawback.
- **C++ exceptions:** only together with Wasm exception handling *and* only on a
  backend that can emit CFI; never on LibJIT as it stands.
- **Defer stack:** on the first concrete audited-helper need (TRAP-4).

Revision 3 landed TRAP-3, so the POD-only rule is **gone**: [`SAFETY.md`](SAFETY.md)
§2.6 now states the narrower contract (a helper reports and returns; a wrapped
native import may throw). This section is kept as the record of why the old rule
existed, which parts of the tree violated it, and what had to change for it to be
removed.

## 11. Relation to `HostBridge` and host adapters

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
| per call | a per-call handle/root arena cleared on normal return and on trap | not shipped; the defer stack (§5) or the import body itself (§9) would make it possible |
| per reference | `HostBridge::references.retain` / `release` through `refClone`/`refStore`/`refDrop` barriers | shipped, but only for counted reference slots |

A per-call arena is especially useful for a JS/Lua/Python adapter: a host value
temporarily exposed through `externref` does not need to become a long-lived
root if the adapter can release the call's temporary handles at the end of the
call, including when the call traps. Under confinement or the status model, the
arena is an ordinary RAII object in the import body (§9.7) — which is the
simplest correct answer. Under the defer stack it is a cleanup thunk; that thunk
must not call `WASM::trap`, `refDrop`, or re-enter Wasm while trap handling is in
progress. The engine must not assume `instanceDestroying` will repair per-call
state after a trap.

`HostBridge` callbacks themselves must not throw or trap
([`WasmHostBridge.hpp`](../WasmBase/WasmHostBridge.hpp)). A host language
runtime that uses its own `longjmp`/exception mechanism must catch and translate
that at the adapter boundary; otherwise it can jump over engine frames and
bypass the trap state entirely.

Nothing in this note adds a JavaScript or host-language dependency to the
engine. The cleanup contract is a C++ API; the language-specific root/handle
policy belongs to the adapter.

## 12. Implementation plan

The milestone IDs (`TRAP-n`) are local to this note; they are not proposal or
backend milestone IDs, and they do not collide with the `TC-n` milestones of
[`proposals/TAILCALLS.md`](proposals/TAILCALLS.md). Revision 3 shipped TRAP-0,
TRAP-1, TRAP-2 (the helper split; the enumerated list is the contract in
`SAFETY.md` §2.6), TRAP-3 (as the boundary exception of §9.7 rather than a
`TrapScope` bridge), TRAP-4 (the `WASM::defer` stack), TRAP-5 (the
`nativeImport` trampoline, [`ABI.md`](ABI.md) “Writing an import”) and TRAP-7.
TRAP-6 (per-agent state) is **implemented** by the Agent/ExecutionState
refactor; true multi-agent execution remains deferred (§12.1).

| Milestone | Work | Closes / enables | Done when |
|---|---|---|---|
| **TRAP-0 — This note** ✅ | Record the option set, the constraint, the current gap and the backend feasibility study. No runtime change. | — | This note is linked from [`SAFETY.md`](SAFETY.md) §2.6, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1, [`ABI.md`](ABI.md) and the README index. |
| **TRAP-1 — Start trap cleanup** ✅ | Normal C++ boundary around the start function: the call site snapshots kind/message into the slot before teardown, so `~ModuleInstance` and `HostBridge::instanceDestroying` run under ordinary unwinding. | §7 start-function gap | A start-function trap invokes `~ModuleInstance` and `HostBridge::instanceDestroying`; no `unique_ptr` is skipped; the embedder sees the original `TrapKind`. |
| **TRAP-2 — Trap-path audit** ✅ | Publish the audited helper list; split the string/array helpers so the non-trivial work runs in a frame that returns; add the debug tripwire. | §7 helper UB, §9.5 list | No audited helper holds a non-trivial local at a trap point; the list is committed in `SAFETY.md` §2.6; the tripwire is enabled in the test build. |
| **TRAP-3 — Pending-trap status** ✅ | `TrapSlot` pending trap, `reportTrap`, call-site checks, error epilogue, driver check, boundary `WASM::Trap` (§9.7). `TrapScope`/`longjmp` removed. Gated on TRAP-2. | §8.5, §10.4 | No helper or import calls `longjmp`; a helper holding a non-trivial local traps and its destructor runs; `try_table` never catches a trap; a tail-call chain propagates the trap; every embedder and harness call site catches `WASM::Trap`. |
| **TRAP-4 — Cleanup stack** ✅ | Fixed-capacity LIFO cleanup stack (`Elv::Util::DeferStack`), `WASM::defer`/`dismissDeferred`, the drain loop, nesting by saved depth. Only if a TRAP-2 helper needs external-state release. | §5, §11 per-call arena | A helper or import can release a host mutex/arena on both normal return and trap; LIFO and nested scopes are tested. |
| **TRAP-5 — Import contract** ✅ | `WASM::nativeImport<&fn>()` (`WasmBase/WasmNativeImport.hpp`) wraps a typed host function: it holds a `NativeImportScope`, catches `Trap`/`std::bad_alloc`/`std::exception`/`...`, reports the equivalent pending trap (message copied into the slot) and returns a benign value. The raw `reportTrap` + return contract remains for helpers. | §9.2–§9.4, §14 re-entry | A throwing import becomes a trap with its message and kind; `WASM::trap()` inside a wrapped import keeps its kind; the import’s own RAII locals are destroyed (tests in `Test/main.cpp`). |
| **TRAP-6 — Per-agent trap state** ✅ | `ExecutionState` owns the pending trap, pending Wasm exception, tail-call state, defer stack, depth counters and stack limit; `Agent` owns an `ExecutionState`; the generated-code helpers resolve the current state. | §14 multi-agent; [`proposals/THREADS.md`](proposals/THREADS.md) TH-5/T9; [`proposals/STACK_SWITCHING.md`](proposals/STACK_SWITCHING.md) SS-6 | Done: two agents on one host thread observe independent traps, exceptions, tail-call targets, cleanup stacks and stack limits; a host thread's default agent is per-thread. |
| **TRAP-7 — Trap-model review** ✅ | Record the chosen model and the rejected alternatives in [`SAFETY.md`](SAFETY.md) §2.6 and [`THREADSAFETY.md`](THREADSAFETY.md) §10.1; update the README completeness row. | §10.5 | §2.6 cites the decision instead of describing a candidate. |

Sequencing notes:

- TRAP-1 and TRAP-2 were independent of each other and of the model choice; they
  removed the two UB sites (the trapping start function and the string/array
  helpers), and both are shipped.
- TRAP-3/TRAP-4/TRAP-5 are one model, shipped together: status propagation +
  boundary exception, the optional cleanup stack, and the wrapped-import
  trampoline.
- A second backend must adopt that model rather than invent its own trap
  emission; otherwise the trap path is implemented twice with different rules
  ([`LAYERING.md`](LAYERING.md) §4).

### 12.1 TRAP-6 is implemented; multi-agent execution remains deferred

TRAP-6 shipped with the Agent/ExecutionState refactor. The state lives in
`WasmBase/WasmAgent.hpp` and belongs to the execution context:

| State | Owner |
|---|---|
| pending trap (`TrapSlot`: flag, kind, message) | `ExecutionState::trapSlot` |
| pending Wasm exception | `ExecutionState::pendingException` |
| tail-call trampoline target/arguments | `ExecutionState::tailCallState` |
| deferred-cleanup stack | `ExecutionState::deferStack` |
| `wasmCallDepth` / `nativeImportDepth` counters | `ExecutionState` fields |
| stack limit | `ExecutionState::stackLimit` (installed per stack) |

`Agent` owns one `ExecutionState`; a host thread's default agent is created
lazily; `AgentScope` installs another agent for a call. The generated-code ABI
is unchanged: `wasm_trap_slot()`, `wasm_exn_slot()`, `currentTailCallState()` and
`wasm_stack_check()` resolve the *current* state, and `wasm_stack_check()` reads
the state's limit first with `VMContext::stackLimit` as the fallback.

What remains deferred is the **execution model**, not the state: agents do not
run concurrently, there is no scheduler, no multi-mutator GC and no real
`wait`/`notify` table. That is THREADS TH-5/T9, and the guest-lock question is
separate. The isolation claim is covered by the tests listed in §13.

## 13. Test plan

The trap path is mostly observable from the embedder, so most of these tests are
host-side; only the last three need codegen-level assertions.

| Test | What it proves |
|---|---|
| Native import arms a mutex, registers a defer, then traps | the unlock thunk runs exactly once before the embedder sees the trap (TRAP-4) |
| Same import returns normally | `dismiss` removes the record; the thunk does not run later |
| Two nested defers | execution order is LIFO |
| Nested boundaries | an inner trap drains only the inner snapshot; outer cleanup survives (nested `callCallable` / `NativeImportScope`) |
| Missing/duplicate dismiss in a debug build | the contract violation is detected rather than silently leaking |
| Cleanup thunk calls `WASM::trap` or Wasm | the engine refuses recursive dispatch; it never re-enters generated frames |
| Import holds `std::lock_guard`, re-enters Wasm, inner call traps | the mutex is released and the outer trap reaches the embedder with the original kind and message (TRAP-5 / §9.7) |
| Same, with a `std::unique_ptr` whose destructor increments a counter | the counter is incremented exactly once before the embedder sees the trap |
| Import calls `WASM::trap()` directly (debug build) | the tripwire asserts |
| Import calls generated code without the guarded/checking entry and it traps (debug build) | the tripwire asserts |
| Nested guarded calls two levels deep, inner trap | only the inner scope catches; the outer scope is restored and sees the re-raised trap |
| Trap message longer than the inline buffer | it is truncated and NUL-terminated, not overrun |
| Import body throws a C++ exception | converted to a trap or aborts, per the chosen policy; never unwinds into generated frames |
| Start function traps after a fake host adapter retains a reference | `~ModuleInstance` runs, `instanceDestroying` runs, the host count returns to zero, and the embedder observes the original `TrapKind` (TRAP-1) |
| Start function traps, and `instanceDestroying` reads the execution state's trap state | the snapshot-before-teardown ordering in TRAP-1 is correct; the propagated kind is not corrupted by host code running during teardown |
| Start function traps before any host reference exists | the partial instance is destroyed and the trap reaches the embedder with the original kind |
| Existing `assert_uninstantiable` case (`start.wast`) | the expected start trap still reports `TrapKind` correctly |
| A helper holding a non-trivial local traps (stringref constructor with an invalid encoding) | its destructor runs; the embedder sees the original kind (TRAP-2/TRAP-3 — this test fails today) |
| A trap inside a `try_table` region | the trap is not caught by any handler and reaches the embedder (trap-first propagation) |
| A trap inside a tail-called chain | it is propagated after an arbitrary number of trampoline re-dispatches, and the trampoline's pending target is cleared |
| A trapping native import called from generated code | the call site's check sees the pending trap and the embedder sees the kind (this is the test that catches a forgotten call-site check) |
| Full suite under ASan/valgrind where available | the trap-path changes introduce no use-after-free or leak |
| `a trapping start function unwinds and destroys the partial instance` (shipped) | `~ModuleInstance` and `instanceDestroying` run when the start function traps (TRAP-1) |
| `deferred cleanup runs LIFO on a trap and is dismissed on the normal path` (shipped) | the explicit stack drains once, most recent first, and a dismissed record never runs |
| `a trap leaves the instance usable for the next call` (shipped) | repeated traps do not poison the thread; a fresh instance still runs |
| `Elv scope guards cover exit, success and failure` (shipped) | the generic `ELV_SCOPE_*` utilities used by the model behave as documented |
| `a wrapped native import converts exceptions into a trap` (shipped) | `nativeImport<&fn>()` turns a thrown `std::exception` into `TrapKind::Internal` with its `what()` message, and the import’s own RAII locals are destroyed (TRAP-5) |
| `WASM::trap` inside a wrapped native import keeps its kind` (shipped) | the wrapper catches `WASM::Trap` and re-reports kind + message instead of unwinding generated code |
| `a wrapped native import still receives the caller’s VMContext` (shipped) | the wrapped form is byte-for-byte the raw import ABI (`ctx->hostData` still reaches the body) |
| `agent execution state isolates traps, exceptions, tail calls and deferred cleanup` (shipped) | two agents on one host thread own independent trap/exception/tail/defer/depth state; `AgentScope` restores the previous state (TRAP-6 / SS-6) |
| `each host thread gets its own default agent and execution state` (shipped) | a host thread's default agent is per thread, and a worker's trap does not leak to the main agent (TRAP-6) |
| `the stack limit belongs to the execution state` (shipped) | `wasm_stack_check` reads the current state's limit, with `VMContext::stackLimit` only as the compatibility fallback (SS-6) |

The shipped rows are the ones named in the table. What is still *plan* rather
than coverage: a dedicated case for a trap inside a `try_table` region (the
trap-first property — today it is asserted only indirectly by the `traps` and
`throw` scripts), one for a trap inside a tail-called chain, and one that proves a
call-site check exists for a trapping import. The `callGuarded` rows are obsolete
by construction: `TrapScope` and `callGuarded` were removed with TRAP-3, so
"the inner scope catches" is now "the boundary throws `WASM::Trap` at the
outermost `callCallable`".

## 14. Open questions

Revision 3 answered the defer-stack and `TrapScope` questions below by shipping
or removing the mechanism; they are kept so the reasoning is not lost. The remaining
open question is guest-side cleanup (no Wasm `finally` on a trap); per-agent
state (**TRAP-6**) is implemented (§12.1).

**For the defer stack (TRAP-4) — shipped.**

- **Fixed capacity — settled:** 32 records per execution state
  (`ExecutionState::kDeferCapacity` in `WasmAgent.hpp`). `defer()` returns an invalid token instead of allocating when
  the stack is full, and `dismissDeferred()` rejects an invalid token, so
  overflow is observable exactly where the caller can still react to it.
- **Cleanup failure — settled by signature:** the thunk type is
  `void (*)(void*) noexcept`, so a throwing thunk terminates rather than
  unwinding during a trap. There is deliberately no secondary-error channel: the
  contract is that a cleanup thunk must not fail.
- **Reentrancy — partly settled, partly open.** A thunk that traps *during the
  drain* is ignored rather than re-entered: `reportTrap()` returns early once a
  trap is pending, so the original reason survives and the drain cannot recurse.
  A thunk that calls back into Wasm is **not** prevented — the depth counters see
  an ordinary nested call — so the adapter's lock ordering and reentrancy rules
  are still its own responsibility.
- **Host callbacks in cleanup — open, and the embedder's decision:** if a
  host-language runtime's release hook can itself fail, the adapter must decide
  before registering it.
- **Normal-path ergonomics — answered in practice:** `defer` +
  `dismissDeferred` is at most two calls, and RAII remains the default; the stack
  exists for state no object owns.

**For the import contract (TRAP-5) — shipped as `nativeImport`.**

- **Message buffer size — settled:** `kTrapMessageCapacity` is 192 bytes, and
  truncation is silent. The message is free text; the comparison the harness
  makes is against the *kind* ([`SAFETY.md`](SAFETY.md) §2.4).
- **Exception policy — settled:** `WASM::Trap` keeps its kind and message,
  `std::bad_alloc` becomes `OutOfMemory`, any other `std::exception` becomes
  `Internal` with `what()`, and anything else becomes `Internal` with a fixed
  message ([`ABI.md`](ABI.md)).
- **Thunk generation — settled differently:** neither a backend-generated
  wrapper per import nor a value-array import ABI. `nativeImport<&fn>()`
  instantiates a template trampoline at compile time, so the import keeps its
  Wasm signature and nothing is generated at instantiation time. The cost is
  documented: a *capturing* callable cannot be wrapped — use
  `VMContext::hostData`.
- **Embedder entry point — settled:** there is no armed scope left to expose.
  `CallDepthScope` marks "inside generated code" and `NativeImportScope` marks
  "inside an import body"; `WASM::trap()` is legal only outside both, and
  `checkTrap()` decides at the boundary.

**For the pending-trap status (TRAP-3) — shipped.**

- **Boundary-rethrow exactness — settled with two counters, not a flag:**
  `wasmCallDepth() != 0` means a generated frame is live, so `WASM::trap()`
  aborts with a diagnostic rather than unwinding it; at the boundary the counters
  are zero and it throws `WASM::Trap`. A host adapter that calls generated code
  without going through the engine is still the one way to get this wrong.
- **Trap vs. exception precedence — settled:** a trap wins and is never routed
  into `emitExceptionPropagation`, so `try_table` cannot catch one; the call-site
  check runs before the exception check.
- **Tail-call trampoline state — settled:** `reportTrap()` resets it where the
  trap is *reported*, so no driver can re-dispatch a target after a trap.
- **Value-returning helpers — settled per helper:** each audited helper returns a
  benign value and its generated caller branches on the slot; §12 has the
  inventory of what shipped.
- **Cost — measured informally, never benchmarked:** one TLS load plus a
  predictable branch per call/helper site. No test asserts a bound; a benchmark
  belongs with SIMD's T4 layer, not here.
- **Zero results — settled:** a trapping function returns the zero of each
  result type and every consumer tests the pending slot before using a value, so a
  null reference is never mistaken for a live one.

**Cross-cutting.**

- **Guest locks.** Nothing in this note releases a guest lock. A future engine
  needs either a Wasm-level cleanup/`finally` path or an engine-owned lock
  registry; a host defer stack cannot see guest mutexes.
- **Multi-agent state (TRAP-6, implemented).** The pending slot, deferred-cleanup
  stack, counters and tail state belong to an `Agent`/`ExecutionState`; two
  agents on one host thread are isolated. What remains deferred is the execution
  model itself (§12.1, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1,
  [`proposals/THREADS.md`](proposals/THREADS.md) TH-5/T9).
- **Backend parity.** When SLJIT/AsmJit are implemented, the trap model must be
  specified once in [`LAYERING.md`](LAYERING.md) terms (a contract), so that
  neither backend invents a third mechanism.

## 15. References

In-repo:

- [`SAFETY.md`](SAFETY.md) §2.4–§2.6 — traps vs aborts, RAII for locks, and the
  report-and-return contract a generated-code helper must honour; §2.7 for the
  per-agent execution model; §2.8 for `CallStackExhausted` and the checked
  stack limit.
- [`THREADSAFETY.md`](THREADSAFETY.md) §5.6 and §10.1 — host mutex/RAII caveats,
  trap/lock ownership, and the open multi-agent questions.
- [`ABI.md`](ABI.md) — the trap contract at the boundary (`WASM::Trap`), the
  `Callable` layout, native imports and their `nativeImport` trampoline, and the
  external `HostBridge` adapter.
- [`LAYERING.md`](LAYERING.md) §2 and §4 — why a trap-model contract belongs in
  the core and what a backend must honour.
- `WasmBase/WasmTrap.hpp` / `WasmBase/WasmTrap.cpp` — `TrapSlot`,
  `reportTrap`/`trap`, `checkTrap`, `trapPending`/`takePendingTrap`, the
  `WASM::Trap` boundary exception, `CallDepthScope`/`NativeImportScope` and the
  defer stack.
- `WasmBase/WasmModuleInstance.cpp` — `ModuleInstantiator::instantiate`, the
  start-function path, and the `memoryInit`/`tableInit`/segment helpers.
- `WasmBase/WasmExn.hpp` / `WasmBase/WasmExn.cpp` — the pending-exception slot
  (`wasm_exn_slot()`), the shape the trap slot mirrors.
- `WasmBase/WasmString.cpp`, `WasmBase/wasm_atomic.cpp` — engine helper trap
  sites outside the backend.
- `LibJit/LibjitOpcodeDispatcher.cpp` — `emitTrap`/`emitTrapIfNull`,
  `emitPendingExceptionCheck`, `emitReturnZeroes`, `callNativeHelper`, the
  inline bounds/null checks, and the stringref/array helpers of §7.
- `LibJit/LibjitModuleCompiler.cpp` — the per-function driver and
  `callStartFunction`.
- `Test/main.cpp`, `Test/WastScript.cpp`, `Test/TestBackend.cpp` — the embedder
  and harness sites that catch `WASM::Trap`; `start.wast`'s
  `assert_uninstantiable` case.
- [`proposals/JS_INTEGRATION.md`](proposals/JS_INTEGRATION.md) §4.3 — the opt-in
  host bridge that a per-call host arena would extend.
- [`proposals/EXCEPTIONS.md`](proposals/EXCEPTIONS.md) — the shipped Wasm
  exception model whose pending-slot shape §8.5 reuses.
- [`proposals/TAILCALLS.md`](proposals/TAILCALLS.md) — the trampoline and
  per-function driver that §9.7 uses as the boundary.
- Go language reference, “Defer statements” and “Handling panics” — the
  inspiration, not an ABI dependency.

Backend evidence (checked for revision 2):

- GNU LibJIT headers and manual on this machine: `/usr/local/include/jit`
  (`jit-insn.h` for `jit_insn_throw`/catchers/finally, `jit-unwind.h` for the
  frame walk, `jit-except.h` for the pointer-exception API) and the exported
  `_jit_unwind_push_setjmp`/`_jit_unwind_pop_setjmp` symbols of
  `libjit.so.0.0.1`.
- SLJIT: `sljitLir.h` (enter/return, labels, arbitrary jumps to handlers, no
  unwind API) and the project README.
- AsmJit: API reference for `FuncDetail`/`FuncFrame` (prolog/epilog insertion, no
  unwind API) and issue #313, “Is it possible to have debug symbols/stack
  unwinding for debugging?” (<https://github.com/asmjit/asmjit/issues/313>),
  closed by the maintainer on 2025-09-25.
- Wasmtime PR #11592, “Replace setjmp/longjmp usage in Wasmtime”
  (<https://github.com/bytecodealliance/wasmtime/pull/11592>, merged 2025-09-08) —
  the production precedent in §8.7.
