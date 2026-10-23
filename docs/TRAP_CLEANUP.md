# Trap Cleanup — Escaping the POD-Only Trap Path

Status: design note / open design item (revision 2 — folds the candidate designs
that were previously appended as revisions into the numbered sections, adds a
feasibility study of the option set against the three backends in the tree, and
records the gating conditions for each option; **no runtime change is
implemented**). This is not a WebAssembly proposal note and it does not change
the current `TrapScope` contract.

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

The question this note answers is not "how do we make `longjmp` run C++
destructors" — nothing can — but **which of the available mechanisms lets the
engine and its embedders stop reasoning about skipped frames, and what does each
one cost on the backends we actually have.** Three families are on the table:

- an explicit, fixed-capacity Go-style cleanup stack that `WASM::trap()` drains
  before it jumps (§5–§6);
- a confinement rule that keeps `longjmp` but shrinks the set of frames it may
  cross to something small and auditable (§9);
- a trap model with no non-local exit at all, in which generated code reports a
  status the way the engine already does for Wasm exceptions (§8.5, §10).

§10 studies each family — plus the C++-exception and signal variants — against
GNU LibJIT, SLJIT and AsmJit, and lists the redesign each one forces.

## 1. Summary and document map

- `WASM::trap()` is a non-local exit, not a C++ throw. C++ RAII works on normal
  return and C++ exceptions, but not across `longjmp`.
- The closest useful analogue to Go's `defer` is a LIFO cleanup stack attached
  to the per-thread trap state. `TrapScope` is the panic target; the stack is
  what runs on the way out. The cleanup records must be preallocated and
  non-throwing, and normal returns must dismiss them explicitly — no C++
  compiler support can make a `defer` statement automatic in a trap-skipped
  frame.
- A defer stack **does not relax** the POD-only rule. It gives native code a way
  to release *external* state (mutexes, arenas, handles) before the jump; it does
  not make non-trivial automatic objects safe in skipped frames.
- The bookkeeping belongs in thread-local state, not in a normal `TrapScope`
  member whose fields change after `setjmp()`. C leaves changed non-`volatile`
  automatics local to the `setjmp()` frame indeterminate after `longjmp`.
- There is a concrete current gap: a trapping start function can jump out of
  `ModuleInstantiator::instantiate` while it holds a
  `std::unique_ptr<ModuleInstance>`, so the instance destructor and the
  `HostBridge::instanceDestroying` callback do not run (§7). That path needs a
  normal C++ boundary (or a trivially destructible holder) under *every* option
  in this note.
- **The POD-only rule is already violated in the tree.** The string and array
  helpers that generated code calls hold `std::vector` locals and then call
  `WASM::trap()` (for example `wasm_string_new_utf8` in
  `LibJit/LibjitOpcodeDispatcher.cpp`). Those frames are skippable today, so the
  destructor is already skipped on the trap path. Whatever option is chosen,
  those helpers have to be restructured — this is not speculative cleanup.
- **The engine already has a pending-failure protocol.** Wasm `throw` sets a
  pending slot (`g_wasmPendingException`), returns zeroes from the generated
  function, and every call site checks the slot (`emitPendingExceptionCheck`).
  Generated-code traps also funnel through two emitter helpers (`emitTrap` and
  `emitAbort`), and the operand stack lives in frame locals rather than a
  machine stack. Extending that protocol to traps is a bounded change, not a
  codegen rewrite (§10.3).
- **No backend in the tree can emit unwind information.** LibJIT has no CFI
  API and its own exception facility is internally `setjmp`-based; SLJIT and
  AsmJit expose no unwind/EH API either (AsmJit's maintainer closed the
  `.eh_frame` request in September 2025). A C++-exception trap model would need
  a hand-written unwind-info generator on top of a backend, and on LibJIT it is
  not reachable at all (§10.1).
- Revised recommendation (§10.5): fix the start-function path first (TRAP-1),
  then move the trap path onto a pending-trap status behind the existing
  `TrapScope` API, keeping at most one audited `longjmp` at the embedder
  boundary (§9.7). The defer stack stays as the fallback for an audited helper
  that must hold external state across a trap. Do not replace `longjmp` with C++
  exceptions or signals.

| Section | Question it answers |
|---|---|
| §2 | What is the trap contract today? |
| §3 | Why does RAII stop at a trap? |
| §4 | What does Go's `defer` actually buy? |
| §5 | What would a trap-cleanup stack look like? |
| §6 | What can it clean, and what can it not? |
| §7 | What is broken today? |
| §8 | What are the alternatives, and which are cures vs workarounds? |
| §9 | How do we keep `longjmp` but confine it? |
| §10 | Which option can the three backends support, and what must be refactored? |
| §11 | How does this interact with `HostBridge`? |
| §12 | What is the plan? |
| §13 | What proves it? |
| §14 | What is still open? |
| §15 | References |

## 2. The trap contract we have

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

That is the whole cleanup machinery today. Two properties matter for everything
below:

- **Two ways in.** Generated code raises a trap through the C-callable
  `wasm_trap_raise(int kind, const char* message)`; engine helpers call
  `WASM::trap(kind, detail)` directly. The kind travels as an `int` in the
  generated-code path because a scoped enum cannot be passed through a backend
  function signature; the message is a constant string, interned per instance.
- **No unwind tables.** The generated frames are not described to the platform
  unwinder, so a C++ exception thrown in a helper frame cannot propagate through
  them. This is why the trap is a `longjmp` and why every frame between the trap
  and the scope is required to be POD-only ([`SAFETY.md`](SAFETY.md) §2.6).

This is not a stylistic preference: if a frame between the trap point and the
`TrapScope` contains an automatic object with a non-trivial destructor,
`longjmp` leaves that destructor unrun and the behavior is not defined by C++.
The rule covers every frame on the path — engine helpers, import bodies and
generated code alike.

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
shape is a normal C++ boundary around the start call:

1. do not `longjmp` across the frame that owns the instance;
2. catch the trap in a nested `TrapScope` at a point where the instance holder
   is still a valid C++ local;
3. snapshot the trap kind and message **before** destroying anything, because
   teardown (`~ModuleInstance`, `instanceDestroying`, adapter callbacks) may
   itself record or clear thread-local trap state;
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
| Keep the current POD-only rule | no new runtime mechanism; the existing contract is honest | host locks/arenas still leak or deadlock on a trap; the start-function leak remains; the string/array helpers are already UB (§7) | status quo, not sufficient |
| C++ scope guards / `std::scope_exit` | familiar RAII syntax | destructors are exactly what `longjmp` skips; no improvement | does not solve it |
| Go-style fixed-capacity cleanup stack (§5) | explicit trap-time release of host locks, arenas and handles; no backend or ABI change | normal-path dismiss discipline; fixed cap; callbacks must be no-throw/non-reentrant; still cannot make non-trivial locals safe | **pragmatic workaround**, keep as fallback |
| Per-import `setjmp` boundary (§8.3) | import-local cleanup of non-trivial locals in the import's own frame | `setjmp` cost on every participating import; re-raise discipline; does not fix engine-internal frames | **narrow workaround**, interchangeable with the defer stack |
| Confinement (§9) | most frames become unskippable, so native import bodies and `instantiate` may use ordinary C++ | import-ABI change or a backend-generated wrapper per import; a closed audited-helper list; a `callGuarded` per re-entry unless §9.7 is used; the rule must be policed in review | **recommended intermediate step**, M on each backend (§10.2) |
| Status-return traps (§8.5) | removes the POD-only rule; ordinary RAII everywhere, including engine frames; no non-local exit at all | helper ABI conversion, an error epilogue per function, a check per call/helper site; a per-call-site branch (predict-not-taken) and extra code size | **cure; long-term direction**, gated (§10.5) |
| C++ exceptions with unwind tables (§8.4) | real C++ unwinding and RAII | no backend emits CFI; needs a documented ABI, a personality routine, `.eh_frame` registration and platform-specific work per backend; harder to debug across generated frames | not recommended |
| Signals / SEH (§8.6) | catches hardware faults without instrumenting the instruction | Wasm traps here are explicit software checks, not faults; handlers are async-signal-unsafe and cannot run destructors either | rejected |
| Engine-owned lock/resource registry | can release or poison host and guest locks after an agent dies; supports cancellation | needs per-agent ownership tracking, poisoning semantics, and a policy for partially executed critical sections | separate design item, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1 |
| Reuse the Wasm exception machinery for traps | one unwinding design; `try_table`/tags already exist | a trap must be *uncatchable* by `try_table`, so the two paths stay distinct in codegen; still needs the status plumbing | a subset of §8.5, not a separate option |

Footnote to the table: workarounds (defer stack, per-import `setjmp`,
confinement) bound the damage of the current `longjmp` model; cures (status
returns, C++ exceptions) change the model. TRAP-1 is independent of both and is
required under the current model either way.

### 8.3 Per-import `setjmp` boundary (narrow workaround)

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
`g_wasmPendingException`, returns zeroes from the current function
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
   `{kind, message}` in the thread-local trap state and *returns* — the
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
thread-local and trap-first relative to the Wasm exception slot, and the
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
| `setjmp`/`longjmp` (today) | **broken** (UB for non-POD) | zero | low | poor: manual `defer` or nested scopes |
| Defer stack | none in skipped frames; external state released | zero | none | moderate: register/dismiss discipline |
| Confinement | full, outside the audited set | zero | low–medium: import thunk or call-site check | good: ordinary C++ in import bodies |
| Pending-trap status | **full**, everywhere | one predict-not-taken branch per call/helper site | medium: epilogue + check, reusing existing machinery | good: `TrapScope` unchanged under §9.7 |
| Status in the return value | **full**, everywhere | call-site branch + wider returns | high: ABI, tail calls, multi-value, imports | good, but every import signature changes |
| C++ exceptions | **full**, everywhere | zero on the happy path | very high: CFI, personality, registration, per platform | good, where it works at all |

### 10.3 What the tree already gives us

The estimate changes materially once the existing code is counted, because
LibJIT already implements the propagation half of the pending-slot design for
Wasm exceptions. Concretely, in `LibJit/` and `WasmBase/`:

| Fact | Where | Why it matters |
|---|---|---|
| A pending-failure slot that crosses module and function boundaries | `g_wasmPendingException` (`WasmExn.cpp`) | the trap status can reuse the pattern and the TLS discipline |
| Generated functions return zeroes when a failure is pending | `emitReturnZeroes()` | a trap epilogue already exists in concept: "return the zero of each result type" |
| Every call site already checks a pending slot | `emitPendingExceptionCheck()` after `call`, imported `call`, internal `call`, `call_indirect` | adding a trap check is one more comparison at the same four emission points, plus the helper calls |
| All generated-code traps funnel through two emitter helpers | `emitTrap()` / `emitAbort()` (`LibjitOpcodeDispatcher.cpp`) | the *failure target* changes; the 21 inline checks that call them do not |
| The Wasm operand stack lives in frame locals, not on a machine stack | `newV128Slot`, `pushValue`/`popValue`, `ControlBlock` slots | the error epilogue has nothing to unwind — no stack-depth bookkeeping, no `alloca` discipline |
| A per-function entry trampoline already exists | the tail-call driver in `LibjitModuleCompiler.cpp` | the §9.7 boundary is a place the engine already generates |
| ~49 native-helper call sites (`jit_insn_call_native`), ~21 inline `emitTrap` sites | `LibjitOpcodeDispatcher.cpp` | the size of the mechanical conversion, and it is mechanical |
| ~55 `trap()` calls inside helper frames | `LibjitOpcodeDispatcher.cpp` | each is a candidate for "report + return"; the count is bounded and known |
| Helpers holding non-POD locals at a trap point | the stringref/array helpers (§7) | the POD-only rule is *already* violated; part of the work is required whatever is decided |
| Ten embedder/harness `setjmp` sites | `Test/main.cpp`, `Test/WastScript.cpp`, `Test/TestBackend.cpp` | the blast radius of changing the embedder contract — and the reason §9.7 keeps `TrapScope` |
| Two failure mechanisms already coexist | `longjmp` for traps, pending slot + return for Wasm exceptions | the duplication Wasmtime cited as a reason to move is already here |

The last row deserves emphasis. The engine implements the Wasm exception
proposal with a *status* model (`throw` becomes a pending payload plus a normal
return) while traps use `longjmp`. Every argument in §8.7 about dual unwinding
mechanisms therefore applies to this tree today, not hypothetically.

### 10.4 The refactoring each option needs

**Defer stack (§5).** `WasmBase/WasmTrap.hpp/.cpp` only: the fixed-capacity
record array and its top in `TrapState`, `TrapScope` saving depth before
`setjmp`, `defer`/`dismiss`, the drain loop and the re-entrancy guard, plus debug
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

**Pending-trap status (§8.5, the §9.7 shape).** The concrete work list:

| File | Change |
|---|---|
| `WasmBase/WasmTrap.hpp/.cpp` | add a pending trap to `TrapState`; add `reportTrap(kind, detail)` (returns) and `trapPending()`/`clearTrapPending()`; keep `trap()` `[[noreturn]]` only for the boundary/abort path, implemented as `reportTrap()` + the bridge jump; `TrapScope` keeps `buffer`/`kind()`/`message()` |
| `WasmBase/WasmModuleInstance.cpp` | convert the ~25 `trap(...)` sites in `memoryInit`/`tableInit`/`bufferInitFrom*`/GC allocation to report-and-return (each function gains a status); the start-function call returns a status so `instantiate` can `instance.reset()` |
| `WasmBase/WasmString.cpp`, `WasmExn.cpp`, `wasm_atomic.cpp` | same conversion for the remaining helper trap sites |
| `LibJit/LibjitOpcodeDispatcher.cpp` | `emitTrap`/`emitAbort` become "record the trap, branch to this function's error epilogue"; add `emitPendingTrapCheck()` after helper calls and at the four existing `emitPendingExceptionCheck()` sites; drop `JIT_CALL_NORETURN` from `wasm_trap_raise` except on the boundary path; make exception propagation trap-first so `try_table` never catches a trap |
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

Recommended order:

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
5. **TRAP-5** — the import contract of §9.2/§9.3, only for a backend that cannot
   emit a call-site check, or as a temporary hardening step while TRAP-3 is in
   flight.
6. **TRAP-6** — per-agent trap state, with
   [`THREADSAFETY.md`](THREADSAFETY.md) §10.1.

When to revisit each option:

- **Status-return traps:** revisit if any of (a) an engine-internal frame other
  than the start function must hold a non-trivial local across a trap-capable
  call; (b) a Wasm exception-handling design is adopted, so a single failure
  mechanism is required; (c) the defer stack's discipline proves to be a
  recurring source of review bugs; (d) a second backend (SLJIT/AsmJit) starts
  emitting traps, so the model is chosen once instead of twice. Conditions (a)
  and (b) are **already true** in this tree — that is why TRAP-3 is recommended
  rather than merely recorded.
- **Per-import `setjmp`:** at TRAP-5 time, compared against the defer stack on
  the first real import, not chosen abstractly. §9.7 removes its main drawback.
- **C++ exceptions:** only together with Wasm exception handling *and* only on a
  backend that can emit CFI; never on LibJIT as it stands.
- **Defer stack:** on the first concrete audited-helper need (TRAP-4).

The POD-only rule in [`SAFETY.md`](SAFETY.md) §2.6 remains in force until TRAP-3
lands. This section does not relax it; it records why the rule exists, what the
tree already does wrong, and what has to change for it to be removed.

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
[`proposals/TAILCALLS.md`](proposals/TAILCALLS.md). Nothing in this table is
committed except TRAP-0.

| Milestone | Work | Closes / enables | Done when |
|---|---|---|---|
| **TRAP-0 — This note** | Record the option set, the constraint, the current gap and the backend feasibility study. No runtime change. | — | This note is linked from [`SAFETY.md`](SAFETY.md) §2.6, [`THREADSAFETY.md`](THREADSAFETY.md) §10.1, [`ABI.md`](ABI.md) and the README index. |
| **TRAP-1 — Start trap cleanup** | Normal C++ boundary around the start function; the `setjmp` lives in a POD-only helper; snapshot kind/message before teardown; reset the holder before re-raising. | §7 start-function gap | A start-function trap invokes `~ModuleInstance` and `HostBridge::instanceDestroying`; no `unique_ptr` is skipped; the embedder sees the original `TrapKind`. |
| **TRAP-2 — Trap-path audit** | Publish the audited helper list; split the string/array helpers so the non-trivial work runs in a frame that returns; add the debug tripwire. | §7 helper UB, §9.5 list | No audited helper holds a non-trivial local at a trap point; the list is committed in `SAFETY.md` §2.6; the tripwire is enabled in the test build. |
| **TRAP-3 — Pending-trap status** | `TrapState` pending trap, `reportTrap`, call-site checks, error epilogue, driver check, boundary bridge (§9.7). `TrapScope` unchanged. Gated on TRAP-2. | §8.5, §10.4 | No helper or import calls `longjmp`; a helper holding a non-trivial local traps and its destructor runs; `try_table` never catches a trap; a tail-call chain propagates the trap; the existing `TrapScope` tests pass unchanged. |
| **TRAP-4 — Cleanup stack** | Fixed-capacity LIFO cleanup stack in the trap state, `defer`/`dismiss`, `currentTrapScope()`, the drain loop, nesting by saved depth. Only if a TRAP-2 helper needs external-state release. | §5, §11 per-call arena | A helper or import can release a host mutex/arena on both normal return and trap; LIFO and nested scopes are tested. |
| **TRAP-5 — Import contract** | `ImportResult`/`TrapRequest`, engine-provided thunk (backend-generated wrapper or a value-array ABI), tripwire; migrate in-tree imports off direct `WASM::trap()`. Only for a backend that cannot emit the call-site check. | §9.2–§9.4 | No in-tree import calls `WASM::trap`; an import that re-enters Wasm and traps is tested. |
| **TRAP-6 — Per-agent trap state** | Move the pending trap, the pending Wasm exception, the tail-call state and the tripwire flags from host-thread-local to per-agent state. | §14 multi-agent | Documented in [`THREADSAFETY.md`](THREADSAFETY.md) §10.1. |
| **TRAP-7 — Trap-model review** | Record the chosen model and the rejected alternatives in [`SAFETY.md`](SAFETY.md) §2.6 and [`THREADSAFETY.md`](THREADSAFETY.md) §10.1; update the README completeness row. | §10.5 | §2.6 cites the decision instead of describing a candidate. |

Sequencing notes:

- TRAP-1 and TRAP-2 are independent of each other and of the model choice; they
  remove the two UB sites that exist today. Everything else is a design
  decision.
- TRAP-3 is where the cure lands; TRAP-4 and TRAP-5 are fallbacks that stay
  documented but unscheduled. If TRAP-3 is deferred, TRAP-5 (with a
  backend-generated thunk) is the cheapest way to make native import bodies
  safe in the meantime.
- A second backend must not write its own trap emission before TRAP-3's model is
  chosen; otherwise the trap path is implemented twice with different rules.

## 13. Test plan

The trap path is mostly observable from the embedder, so most of these tests are
host-side; only the last three need codegen-level assertions.

| Test | What it proves |
|---|---|
| Native import arms a mutex, registers a defer, then traps | the unlock thunk runs exactly once before the embedder sees the trap (TRAP-4) |
| Same import returns normally | `dismiss` removes the record; the thunk does not run later |
| Two nested defers | execution order is LIFO |
| Nested `TrapScope`s | an inner trap drains only the inner snapshot; outer cleanup survives |
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
| Start function traps, and `instanceDestroying` reads thread-local trap state | the snapshot-before-teardown ordering in TRAP-1 is correct; the propagated kind is not corrupted by host code running during teardown |
| Start function traps before any host reference exists | the partial instance is destroyed and the trap reaches the embedder with the original kind |
| Existing `assert_uninstantiable` case (`start.wast`) | the expected start trap still reports `TrapKind` correctly |
| A helper holding a non-trivial local traps (stringref constructor with an invalid encoding) | its destructor runs; the embedder sees the original kind (TRAP-2/TRAP-3 — this test fails today) |
| A trap inside a `try_table` region | the trap is not caught by any handler and reaches the embedder (trap-first propagation) |
| A trap inside a tail-called chain | it is propagated after an arbitrary number of trampoline re-dispatches, and the trampoline's pending target is cleared |
| A trapping native import called from generated code | the call site's check sees the pending trap and the embedder sees the kind (this is the test that catches a forgotten call-site check) |
| Full suite under ASan/valgrind where available | the trap-path changes introduce no use-after-free or leak |

The current suite has one trapping start case (`start.wast`) in
`assert_uninstantiable`; it should be extended with an adapter-level cleanup
assertion, not merely a "the trap happened" check. Of the tests above, the
non-trivial-local case fails today (the destructor is skipped), the
start-function cleanup cases fail today (the instance is orphaned), and the
trap-inside-`try_table` case passes today but is exactly the property TRAP-3 can
break — a trap that starts travelling through the pending slot must not be
routed into `emitExceptionPropagation`. Together they turn the reasoning above
into a regression suite.

## 14. Open questions

**For the defer stack (TRAP-4).**

- **Fixed capacity.** What is the cap, and what happens on overflow? A trap that
  cannot run its cleanup is worse than a normal-path allocation failure.
- **Cleanup failure.** If a thunk must not throw/trap, should a failure be
  `std::abort`, a recorded secondary error, or a poisoned instance? There is no
  safe general recovery path.
- **Reentrancy.** Can a cleanup callback release a host lock that another host
  thread needs in order to make progress, and can that deadlock trap handling?
  The adapter must define its lock ordering.
- **Host callbacks in cleanup.** If a host-language runtime's release hook can
  itself fail, the adapter must decide before registering it.
- **Normal-path ergonomics.** Largely moot under confinement or the status model
  (§9.9); still relevant if the defer stack ships first.

**For confinement and the import contract (TRAP-5).**

- **Message buffer size.** Is 96 bytes enough for the existing `trap()` detail
  strings, and should truncation be observable to the embedder?
- **Exception policy.** Convert an escaping body exception to a trap, or abort?
- **Thunk generation.** A backend-generated wrapper per import, or a value-array
  import ABI? The first keeps `Callable` as it is but puts codegen in the
  instantiation path; the second changes [`ABI.md`](ABI.md) and every import.
- **Embedder entry point.** Should the embedder-facing call also become a
  guarded call, leaving a raw `TrapScope` as the low-level primitive only?

**For the pending-trap status (TRAP-3).**

- **Boundary-rethrow exactness.** How does the bridge know it is at the embedder
  boundary rather than inside an import body? A counter, a flag, or a different
  entry function — and what happens if a host adapter calls generated code
  without telling the engine?
- **Trap vs. exception precedence.** A trap must win over a pending Wasm
  exception and must never be catchable by `try_table`. Confirm that no
  combination (`throw` inside a trapping helper, `throw_ref` of a payload whose
  destructor traps) routes the two through the same slot.
- **Tail-call trampoline state.** `trap()` resets the pending target today. Under
  the status model the reset has to happen where the trap is *reported*, not
  where it is *observed*, or a driver may re-dispatch after a trap.
- **Value-returning helpers.** Which audited helpers can return a status
  cheaply, and which would need generated-code changes to branch on one? The
  count in §10.3 is a first estimate; the per-helper shape is not.
- **Cost.** One TLS read plus a compare per helper call. Measure before assuming
  it is negligible; the branch is predictable but the load is not free.
- **Zero results.** A trapping function whose result is a reference returns a
  null reference; confirm that no consumer (GC barriers, `externref`
  round-trips, multi-value buffers) treats the zero as a live value.
- **`TrapScope` lifetime.** If `TrapScope` keeps `setjmp` for compatibility, does
  it stay correct when a trap is reported on a different thread than the one
  that armed it? Today the state is thread-local and the contract is one thread
  per instance ([`SAFETY.md`](SAFETY.md) §2.7).

**Cross-cutting.**

- **Guest locks.** Nothing in this note releases a guest lock. A future engine
  needs either a Wasm-level cleanup/`finally` path or an engine-owned lock
  registry; a host defer stack cannot see guest mutexes.
- **Multi-agent.** Per-thread trap state is sufficient today, but an agent
  sharing a host thread needs its own trap target, pending slot and cleanup
  stack ([`THREADSAFETY.md`](THREADSAFETY.md) §10.1).
- **Backend parity.** When SLJIT/AsmJit are implemented, the trap model must be
  specified once in [`LAYERING.md`](LAYERING.md) terms (a contract), so that
  neither backend invents a third mechanism.

## 15. References

In-repo:

- [`SAFETY.md`](SAFETY.md) §2.4–§2.6 — traps vs aborts, RAII for locks, and the
  POD-only trap path this note starts from; §2.7 for the thread-local execution
  model; §2.8 for `CallStackExhausted` and the checked stack limit.
- [`THREADSAFETY.md`](THREADSAFETY.md) §5.6 and §10.1 — host mutex/RAII caveats,
  trap/lock ownership, and the open multi-agent questions.
- [`ABI.md`](ABI.md) — `TrapScope` usage, the `Callable` layout, native imports,
  and the external `HostBridge` adapter.
- [`LAYERING.md`](LAYERING.md) §2 and §4 — why a trap-model contract belongs in
  the core and what a backend must honour.
- `WasmBase/WasmTrap.hpp` / `WasmBase/WasmTrap.cpp` — `TrapScope`, `TrapState`,
  `trap()`, `wasm_trap_raise` and the tail-call reset.
- `WasmBase/WasmModuleInstance.cpp` — `ModuleInstantiator::instantiate`, the
  start-function path, and the `memoryInit`/`tableInit`/segment helpers.
- `WasmBase/WasmExn.hpp` / `WasmBase/WasmExn.cpp` — the pending-exception slot
  and the status shape a trap status would mirror.
- `WasmBase/WasmString.cpp`, `WasmBase/wasm_atomic.cpp` — engine helper trap
  sites outside the backend.
- `LibJit/LibjitOpcodeDispatcher.cpp` — `emitTrap`/`emitAbort`,
  `emitPendingExceptionCheck`, `emitReturnZeroes`, `callNativeHelper`, the
  inline bounds/null checks, and the stringref/array helpers of §7.
- `LibJit/LibjitModuleCompiler.cpp` — the per-function driver and
  `callStartFunction`.
- `Test/main.cpp`, `Test/WastScript.cpp`, `Test/TestBackend.cpp` — the embedder
  and harness `TrapScope` sites; `start.wast`'s `assert_uninstantiable` case.
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
