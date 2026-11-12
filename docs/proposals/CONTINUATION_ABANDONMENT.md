# Continuation Abandonment {#prop_continuation_abandonment}

*What a parked fiber owns, what the engine releases, and the plan for the rest*

Status: design note / implementation reference (revision 2 — 2026-11-11: **closed**).
The instance releases the continuations it created when it is destroyed, and the
parked-switch wait wakes on cancellation, so a worker's destructor can join a parked
host thread. The fifteen-case cell and the full 822-case ASan cell are clean on all
three backends; revision 1 records the root cause and the four attempts it took.
Audience: anyone touching `WasmBase/WasmContinuation.{hpp,cpp}`, the continuation
lowerings in `Asmjit/AsmjitModuleCompiler.cpp`, `Sljit/SljitOpcodeDispatcher.cpp`
and `LibJit/LibjitOpcodeDispatcher.cpp`, or the sanitizer gate in
[`TESTING.md`](../TESTING.md).

This note follows the same split as [`GC.md`](GC.md) and
[`STACK_SWITCHING.md`](STACK_SWITCHING.md): what the runtime has to guarantee,
what it does not, what is implemented, the gaps, the deviations we may accept,
the plan, the test survey, the references. It contains **no new code**; the
defect it records is closed in the register as `GAP-19`.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What must the runtime guarantee when a continuation is abandoned? |
| §3 | What does the guarantee deliberately *not* demand? |
| §4 | What does the tree do today? |
| §5 | Where does it fall short, and what is the evidence? |
| §6 | Which deviations are acceptable while closing §5? |
| §7 | What is the plan that closes §5? |
| §8 | What did the recorded runs actually say? |
| §9 | References |

## 2. What the runtime has to guarantee

- **A continuation is a handle, not a GC object.** `(ref cont)` is a
  refcounted 32-byte `Continuation` that holds a `std::shared_ptr<Worker>`; the
  worker owns the native stack, the machine context and the argument/result
  slots. Several handles may name one worker; the one-shot rule is enforced by
  the worker's generation.
- **A suspended continuation is a live native stack.** It is not a GC-scanned
  root ([`ABI.md`](../ABI.md) “Call forms”), so its contents are not traced; the
  only mechanism that frees it is the last handle's release.
- **`resume`, `switch` and `cont.bind` consume their operand.** Ownership of the
  operand transfers to the callee, which is why every lowering emits a release
  for it.
- **Reference locals own one count each.** A compiled prologue retains its
  reference parameters and the ordinary epilogue releases every reference local
  before returning (`emitDropReferenceLocals` in AsmJIT, the equivalent slot
  drops in SLJIT and LibJIT).
- **Therefore**: a fiber that is *abandoned* — parked at `switch` or `suspend`
  and never resumed, its handles dropped — must still release everything its
  frames own. Otherwise the guest can grow the host's memory without bound by
  parking continuations in a loop, which is exactly what
  [`SAFETY.md`](../SAFETY.md) exists to prevent.

## 3. What the guarantee does not demand

- **Promptness.** Reclamation may happen at the worker's destruction rather than
  at the moment of abandonment; no guest-observable timing is promised.
- **Detection.** The runtime need not know *that* a fiber was abandoned; it only
  must not leak when it was.
- **A policy for a deliberate host-held handle.** An embedder that keeps a
  suspended continuation alive is not leaking — it is using the handle.
- **Any particular representation.** Slot maps, arenas or an eager sweep are all
  acceptable; the note requires only that the counts come back.

## 4. What is implemented

**Snapshot (2026-11-11, `a608f4a`):** the instance owns the continuations it creates.

- `ModuleInstance::~ModuleInstance` calls `releaseContinuationsFor(&ctx)` before it
  withdraws its callable ranges. Every `Continuation` whose worker was created in
  that instance is removed from the registry and deleted, while the store and the
  instance's own storage are still intact (`WasmBase/WasmContinuation.{hpp,cpp}`).
- Deleting the last handle destroys the `Worker`, and that destructor is what
  releases: `AWorker` resumes the parked fiber with the cancel flag
  (`cancelSuspended()`), `BWorker` sets `cancelRequested`, marks the worker
  `Cancelled`, notifies its condition variable and joins its thread.
- `BWorker::resumeSwitchPeer` waits for the caller to leave `Running` **or** for
  `cancelRequested`, and answers `Invalid` in the cancelled case. A cancelled
  caller never becomes `Running` again, so without that exit the join waits for
  ever — the defect the fifth attempt found.
- `wasm_cont_resume_ops`, `bind_ops`, `suspend_ops` and `switch_ops` still consume
  their operand, and the backends still emit the operand's release after the call.
  That release sits on the path a parked fiber reaches only when it resumes, which
  is why the instance-wide release above is what actually guarantees the counts.
- The measured runs are §8.

### 4.1 How it was found (2026-11-11, revision 1)

Five attempts. Each one is kept because it names a fact that is easy to get wrong
again.

- **Attempt 1 — release the operand inside the `_ops` helpers.** The helpers took
  over the release and the backends' post-call drops went away. The total did not
  move by a byte (31,864 B / 66 allocations): the outstanding counts belonged to
  the parked frame's *own* slots (a parameter retain and a `local.get` clone), not
  to the operand.

- **Attempt 2 — record the frame's slots on the worker.** Nothing changed, for the
  same reason in a different form: recording leaves the counts in the slots.

- **Attempt 3 — move the values out of the slots while parked.** The host-thread
  case went from 32 bytes to clean and the others moved only slightly (5,333 /
  10,884 / 10,666). A move shuffles raw pointers and never touches a handle's
  `refs`, so the handle kept the worker alive and `~Worker` — the only place that
  could release the rest — never ran.

- **Attempt 4 — release at `Store::~Store`.** Right shape, wrong place: ASan
  reported a cross-thread use-after-free inside `releaseContinuationsFor` when the
  store was destroyed during static teardown with a host-thread worker still alive.

- **Attempt 5 — release at the instance, with cancellation.** The instance is the
  right owner (no use-after-free), but the first run hung in the join. The missing
  piece was the cancellation exit in the parked-switch wait; with it, the join
  completes and both cells are clean (§8.2).

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~CA1~~ | **A parked frame's reference slots keep their counts for ever.** The frame never resumes, so neither the post-call operand release nor the epilogue runs; the handle it holds pins its worker, and with it the 5,200-byte native stack. The cycle is: frame slot → handle → worker → (the only destructor that could release the slot). | the refcount trace of §4 (net +2, zero releases on one handle); the emitted release exists and never executes; 31,864 B / 66 allocations on AsmJIT and SLJIT | **closed (`CA-1`)** |
| ~~CA2~~ | **Staging buffers inside an abandoned `invokeFfiContinuation` frame.** `params`, `returns`, `frame`, `argumentTypes`, `argumentValues` and `pinned` are function-local containers; an abandoned frame never runs their destructors. | the residual ~472 bytes of the same cell (4 × 1 B, 8 × 16 B, 8 × 32 B allocations) | **closed (`CA-1`)** |
| ~~CA3~~ | **Caller frames are not covered.** A tracking scheme that only records the *parking* frame misses a caller that holds reference locals across the park. | design limit of the prototype in §7; not yet measured separately | **closed (`CA-1`)** |
| ~~CA4~~ | **The aggregate cell number is order-dependent.** Catch2 seeds each run, so the same binary reports different totals; a tree with the prototype reverted re-reported 21,332 B on SLJIT where the same tree had reported 31,864 B. | §8, last two rows | **closed (`CA-3`)** |

All four are closed by revision 2. `CA-2` needed no milestone of its own: the
cancellation path unwinds the abandoned frames, which runs the staging destructors
it described. `CA-3` describes the `--rng-seed` sweep of §8.

## 6. Where we could deviate, and why it is safe

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| `CAD1` | Track the **parking frame's** slots first, not the whole abandoned stack. | The parked frame is the one that owns the values the module was manipulating at the park; a caller with live reference locals across a `switch` is rarer and shows up in per-case numbers. | A contained prologue/park-site change instead of a whole-stack slot map. |
| `CAD2` | Leave LibJIT's lowering untouched. | LibJIT is clean on every recorded cell; its frames complete in these modules, so the post-park release runs. | No churn in a backend that does not exhibit the defect. |
| `CAD3` | Release from `~Worker`, not eagerly at the park. | A parked fiber may legitimately resume, so abandonment is only certain when the last handle goes; the destructor is that point. | No new policy, no premature release, no double drop. |

`CAD1` was the prototype's compromise and did not ship: revision 2 releases every
handle the instance created, so a caller frame is covered without a slot map.

Guardrails on the two that remain: `CAD2` must be revisited the moment a LibJIT case
is found parked and abandoned; `CAD3` requires that a resumed frame take its values
back before the destructor can see them, or both would release the same counts.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CA-1 — Release an instance's continuations** ✅ | `releaseContinuationsFor(VMContext*)` is called from `ModuleInstance::~ModuleInstance`: every handle whose worker was created in that instance is erased and deleted while the store and the instance's storage are intact, so each worker's destructor can release what it still owned. `BWorker::resumeSwitchPeer` wakes on `cancelRequested` as well as on `Running`, which is what lets a parked host thread be joined. | `CA1` | the 15-case cell is clean on all three backends under `--rng-seed 12345`, and so is the full 822-case ASan cell (§8.2) |
| **CA-3 — Deterministic evidence, then close** ✅ | Record every case of the cell separately with a fixed seed, then re-run the full 822-case ASan cell; write the numbers into [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.1 `GAP-19` and §13.5 criterion 4. | `CA4` | `GAP-19` closes with per-case numbers, `GAP-8` becomes met, and the cell joins the required runs |
| **CA-4 — Caller frames** (folded into `CA-1`) | Extend the record to the caller chain only if CA-1's per-case numbers still show a handle outstanding. | `CA1` | no case leaks |

All four are closed: `CA-1` and `CA-3` by their own milestones, `CA-2` and `CA-4` by
`CA-1`'s instance-wide release — it covers caller frames without a slot map, and the
cancellation path unwinds the abandoned frames, which runs the staging destructors.
The five attempts that found this are §4.1; the runs that closed it are §8.2.

## 8. Indicative test survey

`build/Desktop-ASan` is ASan + LeakSanitizer at `-O0` with
`LSAN_OPTIONS=suppressions=Test/asan.supp`; `build/Desktop-Debug` covers the
functional rows. Catch2 seeds each run, so every row names its seed.

### 8.1 The defect (2026-11-11, revision 1)

| Cell | Command | Observed | Reading |
|---|---|---|---|
| 15-case cell, AsmJIT | `WASMJIT_BACKEND=asmjit ./WasmJit 'stack-switching*'` | 31,864 B / 66 allocations, exit 1 (LeakSanitizer); 15/15 cases pass | the defect |
| 15-case cell, SLJIT | `WASMJIT_BACKEND=sljit …` | 31,864 B / 66 allocations | the `_ops` path is shared, so the defect was too |
| 15-case cell, LibJIT | `WASMJIT_BACKEND=libjit …` | exit 0, no report | not reached; LibJIT's frames complete |
| Five leaking cases (AsmJIT) | one case per run | `resume_throw dispatches a handler clause` 5,317 · `switch … native-stack handoff` 10,564 · `switch … host-thread handoff` 32 · `dispatches multiple handlers and a loop handler` 10,634 · `module routes a suspension to its handler` 5,317 | the totals summed to the cell exactly |
| Prototype, SLJIT | as above | 26,596 B / 58 | one handle and its `AWorker` reclaimed |
| Prototype, AsmJIT | as above | 21,396 B / 54, 15/15 pass | worked once the store form was right |
| Reverted tree, SLJIT | as above | 21,332 B / 54 | the order dependence `CA4` described |

### 8.2 The closing runs (2026-11-11, `a608f4a`, `--rng-seed 12345`)

| Cell | Command | Observed | Reading |
|---|---|---|---|
| 15-case cell, all three backends | `WASMJIT_BACKEND=<name> ./WasmJit 'stack-switching*'` | **exit 0**, 90 assertions / 15 cases, no report | the fix |
| Full ASan cell, all three backends | `./WasmJit '~[tail]~[compilation-policy]'` | **exit 0**, 22,639 assertions / 822 cases, no report | `GAP-8` met |
| Full Debug suite, all three backends | `./WasmJit` | **exit 0**, 22,800 assertions / 839 cases | no regression |

## 9. References

- [`ABI.md`](../ABI.md) “Continuations” (the `wasm_cont_*` entry points, status
  codes, the one-shot rule, wide slots) and “Call forms” (the frame is not a GC
  root).
- [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.4 `GAP-19` (the closed-register
  row), §8.5 (reach and consequence for a real module), §13.5 criterion 4 (the
  gate this cell feeds).
- [`STACK_SWITCHING.md`](STACK_SWITCHING.md) `SS<n>` — §4 there is the shipped
  continuation surface this note builds on.
- [`TESTING.md`](../TESTING.md) “Required runs” — the ASan configuration, the
  `~[tail]~[compilation-policy]` exclusion syntax, and the cell measured here.
- [`SAFETY.md`](../SAFETY.md) — why an input-driven unbounded leak is a policy
  failure, not a cosmetic one.
- Source anchors: `WasmBase/WasmContinuation.{hpp,cpp}` (`Continuation`,
  `Worker`, `wasm_cont_*_ops`, `continuationRefRelease`);
  `Asmjit/AsmjitModuleCompiler.cpp` (`dispatchResume`, `dispatchSuspend`,
  `dispatchSwitch`, `emitDropReferenceLocals`,
  `emitReferenceParameterRetains`); `Sljit/SljitOpcodeDispatcher.cpp` (the same
  three dispatches); `LibJit/LibjitOpcodeDispatcher.cpp`
  (`emitContinuationResultHandling`).
- Upstream: the WebAssembly stack-switching proposal (`switch`, `suspend`,
  `resume`, `(ref cont)` ownership) and the conformance scripts under
  `extern/WasmTestsuite`.

