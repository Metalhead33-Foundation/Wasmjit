# Continuation Abandonment {#prop_continuation_abandonment}

*What a parked fiber owns, what the engine releases, and the plan for the rest*

Status: defect study / implementation plan (revision 1 — 2026-11-11: opened by
the first complete AddressSanitizer run of the default suite; records the root
cause, two rejected fixes, a prototyped mechanism that is not yet landed, and
the measurement protocol the closure needs).
Audience: anyone touching `WasmBase/WasmContinuation.{hpp,cpp}`, the continuation
lowerings in `Asmjit/AsmjitModuleCompiler.cpp`, `Sljit/SljitOpcodeDispatcher.cpp`
and `LibJit/LibjitOpcodeDispatcher.cpp`, or the sanitizer gate in
[`TESTING.md`](../TESTING.md).

This note follows the same split as [`GC.md`](GC.md) and
[`STACK_SWITCHING.md`](STACK_SWITCHING.md): what the runtime has to guarantee,
what it does not, what is implemented, the gaps, the deviations we may accept,
the plan, the test survey, the references. It contains **no new code**; the
defect it describes is open in the register as `GAP-19`.

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

## 4. What is implemented today

**Snapshot (2026-11-11, `c9089b3`):** the mechanism that *should* release a
parked frame is emitted, but on a path an abandoned fiber never reaches.

- `wasm_cont_resume_ops`, `wasm_cont_bind_ops`, `wasm_cont_suspend_ops` and
  `wasm_cont_switch_ops` are the frame-form entry points; SLJIT and AsmJIT call
  them, LibJIT calls the `_ex` entry points directly.
- Each backend emits the operand's release **after** the helper call
  (`emitRefDrop(continuation)` in AsmJIT, `wasm_ref_drop` in SLJIT, and
  LibJIT's `emitDropContinuation`/`emitDropValue`). That code is on the
  post-park path.
- `~Worker` releases the values it *copied* (`boundValues`, `tagValues`,
  `resultValues`, `resumeValues`, `roots`). It does not know about values that
  live in a guest frame's slots.
- Measured on the `stack-switching*` cell (15 cases) under ASan: **31,864 bytes
  in 66 allocations** on AsmJIT **and** SLJIT, **clean on LibJIT**.
- Instrumented accounting (`wasm_ref_clone` / `wasm_ref_drop`, one 32-byte case):
  one handle ends at **net +2** — two retains (the callee's parameter retain and
  the `local.get` clone that becomes the operand), **zero releases** — while
  every other handle balances. Its release calls exist in the emitted code
  (unconditional, the first thing after the helper returns) and never appear in
  the call trace: that fiber is never resumed.


## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| `CA1` | **A parked frame's reference slots keep their counts for ever.** The frame never resumes, so neither the post-call operand release nor the epilogue runs; the handle it holds pins its worker, and with it the 5,200-byte native stack. The cycle is: frame slot → handle → worker → (the only destructor that could release the slot). | the refcount trace of §4 (net +2, zero releases on one handle); the emitted release exists and never executes; 31,864 B / 66 allocations on AsmJIT and SLJIT | unbounded growth, never a wrong answer |
| `CA2` | **Staging buffers inside an abandoned `invokeFfiContinuation` frame.** `params`, `returns`, `frame`, `argumentTypes`, `argumentValues` and `pinned` are function-local containers; an abandoned frame never runs their destructors. | the residual ~472 bytes of the same cell (4 × 1 B, 8 × 16 B, 8 × 32 B allocations) | bounded per abandoned fiber |
| `CA3` | **Caller frames are not covered.** A tracking scheme that only records the *parking* frame misses a caller that holds reference locals across the park. | design limit of the prototype in §7; not yet measured separately | unmeasured; may be the residue `CA1` leaves behind |
| `CA4` | **The aggregate cell number is order-dependent.** Catch2 seeds each run, so the same binary reports different totals; a tree with the prototype reverted re-reported 21,332 B on SLJIT where the same tree had reported 31,864 B. | §8, last two rows | evidence, not a defect |

## 6. Where we could deviate, and why it is safe

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| `CAD1` | Track the **parking frame's** slots first, not the whole abandoned stack. | The parked frame is the one that owns the values the module was manipulating at the park; a caller with live reference locals across a `switch` is rarer and shows up in per-case numbers. | A contained prologue/park-site change instead of a whole-stack slot map. |
| `CAD2` | Leave LibJIT's lowering untouched. | LibJIT is clean on every recorded cell; its frames complete in these modules, so the post-park release runs. | No churn in a backend that does not exhibit the defect. |
| `CAD3` | Release from `~Worker`, not eagerly at the park. | A parked fiber may legitimately resume, so abandonment is only certain when the last handle goes; the destructor is that point. | No new policy, no premature release, no double drop. |

Guardrails: `CAD1` is only acceptable while `CA3` stays *recorded as open* and a
per-case measurement backs the claim; `CAD2` must be revisited the moment a
LibJIT case is found parked-and-abandoned; `CAD3` requires that the worker's
record be removed when the frame resumes, or a resumed frame would be released
by both its epilogue and the destructor.

## 7. Plan

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **CA-1 — Give the frame's slots an owner** | Land the mechanism: an `ownedSlots`/`ownedCount` pair on `ContinuationOpFrame`; a per-worker record list; `registerParkedFrame`/`unregisterParkedFrame` around the park inside `wasm_cont_switch_ops` and `wasm_cont_suspend_ops`; a `~Worker` pass that releases whatever a never-resumed frame left behind; and the park-site stores in SLJIT and AsmJIT. **AsmJIT's vendored 1.21 `Mem` has no `base()`** — use the `x86::qword_ptr`/`dword_ptr` form with an explicit size, and note that the unsized form failed 9 of 15 cases at compiler finalisation (`finalisation failed: 52`). | `CA1` | the 15-case cell is clean on AsmJIT and SLJIT under a fixed `--rng-seed`, and the Debug suite is green on all three backends |
| **CA-2 — No allocation in the staging path** | Make `invokeFfiContinuation` / `invokeScalarI32` allocation-free: the frame and the argument arrays are bounded by `ContinuationOpFrame::kCapacity` (16), so fixed arrays suffice. | `CA2` | the same cell reports no staging allocations |
| **CA-3 — Deterministic evidence, then close** | Record every case of the cell separately with a fixed seed, then re-run the full 822-case ASan cell; write the numbers into [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.1 `GAP-19` and §13.5 criterion 4. | `CA4` | `GAP-19` closes with per-case numbers, `GAP-8` becomes met, and the cell joins the required runs |
| **CA-4 — Caller frames, if they remain** | Extend the record to the caller chain only if CA-1's per-case numbers still show a handle outstanding. | `CA3` | no case leaks |

Ordering: CA-1, then CA-3's per-case protocol (it decides whether CA-4 is real
work), with CA-2 independently and CA-4 only on evidence.

## 8. Indicative test survey

Every row is a recorded run, not an intention. `build/Desktop-ASan` is ASan +
LeakSanitizer at `-O0` with `LSAN_OPTIONS=suppressions=Test/asan.supp`;
`build/Desktop-Debug` covers the functional rows.

| Cell | Command | Observed | Reading |
|---|---|---|---|
| 15-case cell, AsmJIT | `WASMJIT_BACKEND=asmjit ./WasmJit 'stack-switching*'` | 31,864 B / 66 allocations, exit 1 (LeakSanitizer); 15/15 cases pass | the defect (`CA1`, `CA2`) |
| 15-case cell, SLJIT | `WASMJIT_BACKEND=sljit …` | 31,864 B / 66 allocations | the `_ops` path is shared, so the defect is too |
| 15-case cell, LibJIT | `WASMJIT_BACKEND=libjit …` | exit 0, no report | not reached; LibJIT's frames complete |
| Five leaking cases (AsmJIT) | one case per run | `resume_throw dispatches a handler clause` 5,317 · `switch … native-stack handoff` 10,564 · `switch … host-thread handoff` 32 · `dispatches multiple handlers and a loop handler` 10,634 · `module routes a suspension to its handler` 5,317 | the totals sum to the cell exactly |
| Prototype, SLJIT | as above | 26,596 B / 58 | one handle and its `AWorker` reclaimed: the mechanism works |
| Prototype, AsmJIT | as above | 21,396 B / 54, 15/15 pass | works once the store form is right |
| Reverted tree, SLJIT | as above | 21,332 B / 54 | **order dependence** (`CA4`) — one sample is never a closure |

Functional coverage while the defect is open: `stack-switching*` is green on all
three backends in Debug (90 assertions / 15 cases each), and LibJIT behaves
identically before and after every experiment above.

## 9. References

- [`ABI.md`](../ABI.md) “Continuations” (the `wasm_cont_*` entry points, status
  codes, the one-shot rule, wide slots) and “Call forms” (the frame is not a GC
  root).
- [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.1 `GAP-19` (the register
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

