# Backend history {#backend_history}

*The dated record: v128 boundary, frame ABI, AsmJit/SLJIT slices, defects*

Status: **historical record (frozen at 2026-11-10)**. This note is the merged
archive of the per-slice post-mortems, the capability probes, the handoff and the
two design campaigns. It explains how the backends got here and what broke on the
way; it is **not** a statement about today. Current status is
[`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §13, the open-item index is §8
there, and the current call-form contract is [`ABI.md`](../ABI.md) “Call forms”.

Read it for the **root causes and the lessons**, not for current numbers.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What landed, in what order? |
| §3 | How did the `v128` and frame-ABI campaigns go? |
| §4 | What did each AsmJit/SLJIT slice add, and what broke? |
| §5 | Which defects were found, and what was the root cause? |
| §6 | What did the reference-ownership and call-form work expose? |
| §7 | Which process lessons are worth carrying forward? |
| §8 | References |

## 2. Timeline

| Date | Slice / event | Outcome |
|---|---|---|
| 2026-11-05 | P0–P2 parity | SLJIT brought up to AsmJIT’s execution surface; one fixture set checks both; traps become pending statuses; unload returns code |
| 2026-11-05 | P2 memory64 | 64-bit loads/stores, `memory.size`/`grow`; bulk memory64 initially refused, then fixed with 64-bit operands |
| 2026-11-05 | P3 scalar float | SLJIT lands first; float scratch count and unordered-compare traps found; AsmJIT follows |
| 2026-11-05 | reference slice 1 | `ref.null`, `ref.is_null`, `ref.eq`; `ref.eq` needs GC values to be meaningful |
| 2026-11-05/06 | `v128` decision | Two scalar words, low first; result through the multi-value buffer; aggregate rejected on MS x64 |
| 2026-11-06/07 | frame ABI migration | Core arithmetic, SLJIT frame, AsmJIT frame, LibJIT frame, call cache; opt-in `setFrameCallAbiOptIn` |
| 2026-11-05/09 | SIMD slices 1–9 | Transport, integer/float lanes, narrowing/widening, swizzle/shuffle; AsmJIT `narrow` defect quarantined then fixed |
| 2026-11-07 | GC slice 1 | `struct.new`/`struct.get` on SLJIT; field width follows type; packed/`v128` fields refused by name |
| 2026-11-07 | sweep + remaining work | D-1…D-3, V-1, capability probes, constant-stack tails, block params, shared atomics |
| 2026-11-09 | ownership + call form | Reference sites closed; mixed graphs work; all three suites equal |
| 2026-11-10 | consolidation | This note absorbs the per-slice reports; the current verdict moves to `API_ABI_STABILITY.md` |
## 3. The `v128` and frame-ABI campaigns

### 3.1 `v128` across the boundary

The first boundary attempt passed a 16-byte aggregate. It was rejected as the
**canonical** rule because MS x64 passes aggregates by reference while SysV,
AAPCS64 and RISC-V pass them in registers: one target diverges. The four
candidates and the reason each failed are in
[`../ABI.md`](../ABI.md) §5.4.

Decision: **two `uint64_t` parameters, low word first; a `v128` result goes
through the existing result buffer as `lo, hi`.** This is the only shape that is
register-passed on all four targets and has a portable C name. The cost is
explicit: no vector register is used, and a vector result costs a store.

Compliance at the time:

| Backend | Parameter | Result | Status |
|---|---|---|---|
| SLJIT | two `W` slots, low first | caller-supplied 16-byte buffer | canonical |
| AsmJIT | two `kUInt64`, low first | caller-supplied buffer | canonical |
| LibJIT | one 16-byte aggregate | aggregate | recorded deviation `D-L1` |

`D-L1`: libjit loses the **sixth integer argument** (`r9`) because the last
SysV register-passed word is corrupted in its code generation. `jit_dump_function`
proved the engine’s IR was correct; the loss was inside libjit. On SysV the
aggregate is the same ABI as two words; on MS x64 it is passed by reference, which
is why it stays a deviation. The opt-in frame form retires it because every value
becomes bytes at an offset.

### 3.2 The frame ABI

The frame form is `void fn(VMContext*, void* frame)`: a caller-allocated,
16-byte-aligned block with arguments first, results second. It was adopted to
retire four register-form limits at once — LibJIT’s sixth word, SLJIT’s four-entry
C-call ceiling, AsmJIT’s vector-result special case and the per-backend spelling
divergence.

Migration order:

1. **Core arithmetic** (`frameValueBytes`, `frameArgumentsBytes`,
   `frameResultsBytes`, `frameArgumentOffset`, `frameResultOffset`, `frameBytes`)
   derived from `argSlotCount`, so frame and register layouts cannot disagree.
2. **Capability + primitive** (`ModuleInstance::frameCallAbi()`,
   `callCallableFrame`), without adding a virtual to the frozen seam.
3. **SLJIT**: frame signature, prologue from frame, call scratch area, result
   copy; the `sljit_get_local_base` crash was a transposed `dstw` argument.
4. **AsmJIT**: signature translation, frame prologue, driver, call cache; the
   last blockers were the driver shape and a branch-order bug where
   `bufferedVectorResult` beat `frameMode` in the trap epilogue.
5. **LibJIT**: frame prologue, calls, native imports, multi-result; the switch is
   **policy, off by default**, because automatic switching broke ten working
   modules (its aggregate spelling already delivers wide vectors).
6. **Call cache**: the embedder never sees a frame; `ModuleCallCache` builds it.

The opt-in is `WASM::setFrameCallAbiOptIn(true)`. A backend must not infer the
form from signature measurements; the migration log is explicit that inference
is the failure mode.
### 3.3 The ABI refactor study and delivery (2026-11-06)

The refactor that produced the current `v128` boundary was preceded by a study and
followed by a post-mortem; both are merged here. The current contract is
[`../ABI.md`](../ABI.md), not this record.

**Blast radius.** Nine layers: ABI definition, core value plumbing, the three
backends, scaffolding backends, the plugin seam, binding tooling, test
infrastructure, ABI baselines and documentation. Four were small; the expensive
ones were the call cache, the three backends and the fixtures (66 files mentioned
`v128`). **Phases:** P0 freeze the decision; P1 pin the contract without a
backend; P2 parity fixtures; P3 core plumbing; P4 one backend at a time (LibJIT,
SLJIT, AsmJIT); P5 optional fast paths (deferred, no measured case); P6
documentation closure (this consolidation).

**What shipped.** SLJIT and AsmJIT emit the two-word `v128`; LibJIT keeps its
aggregate (deviation `D-L1`), and the opt-in frame form retires it; the call cache
describes the matching libffi shape; cross-backend calls are legal by ABI but
forbidden in production until a cross-backend suite exists.

**What went well:** the decision note first, pins before changes, order by what a
change can displace (SLJIT first, LibJIT last), IR-level tests that isolate one
cause per build, `jit_dump_function` instead of guessing, and refusal instead of
corruption.

**What went badly:** four LibJIT attempts crashed before any pin worked because
the plan was written before the code was read; three plan reorderings; the wrong
`translateFunctionSignature` overload hit twice; a shared fixture carrying a
SLJIT-incompatible shape twice; and one round blaming the wrong layer. The
pattern in all of them: **a limit written when there was one call form was applied
to both forms alike.** The rule that would have prevented them: before writing a
guard, grep for the guard; before blaming a layer, grep that layer’s own
constants; before claiming a path is untested, run the test that would prove
either answer.

**At merge:** 21,717 assertions / 746 cases; 11 `[libjit][v128][ir]` pins, two
boundary pins, one cache-flag pin, one refusal pin. **Follow-ups:** the AsmJIT
result buffer shipped; SLJIT stack-passed argument slots were measured as not
implementable from here (the frame form is the supported way); the binding
generator and call cache speak the canonical shape; portability is still SysV
AMD64 only (`GAP-6`).

## 4. The AsmJit/SLJIT slice series

| Slice | What landed | What broke / what it taught |
|---|---|---|
| **P0–P2** | SLJIT matches AsmJIT’s execution surface; shared fixtures; unload frees every entry | A refusal test used an opcode the backend had since learned, so the test had to be re-pointed; traps are statuses, not jumps |
| **P2 memory64** | 64-bit loads/stores and `memory.size`/`grow`; bulk memory64 | Bulk helpers initially took 32-bit operands; the refusal was honest, the fix was to widen them |
| **P3 float** | Scalar `f32`/`f64` on both | `sljit_emit_enter` needs the float scratch count; `SLJIT_F_EQUAL` is “equal or unordered” — `f32.eq(NaN, NaN)` returned 1 until the ordered form was used |
| **GC 1** | `struct.new`/`struct.get` on SLJIT; field width follows the type | Packed and `v128` fields refused by name; `struct.set` blocked by reference-typed block results |
| **refs 1** | `ref.null`, `ref.is_null`, `ref.eq` | `ref.eq` could not be fixtured meaningfully until GC objects existed |
| **SIMD 1** | transport, bitwise, `i32x4` lanes | Helper ABI avoids the aggregate question; AsmJIT needs its own result copy |
| **SIMD 2** | integer lanes, comparisons, `all_true` | Per-shape helpers; no generic “vector op” |
| **SIMD 3** | shifts, saturation, `min`/`max`, `abs`, `popcnt` | Shift-count and signedness corners; one helper per opcode |
| **SIMD 4** | widening, `extmul`, `extadd_pairwise`, `dot`, `narrow` | Lane order in packing instructions is easy to reverse; assert lane placement, not just the numeric result |
| **SIMD 5** | AsmJIT `narrow` defect | The defect was quarantined rather than pinned as expected; the differential test could not see it, the golden lane vector did |
| **SIMD 6** | float lanes | NaN propagation, min/max and conversions need their own helpers; do not generalise from integer lanes |
| **SIMD 7** | float lane access | `extract`/`replace` need scalar-result/argument shapes |
| **SIMD 8** | extending/splat/zero loads, lane load/store, `swizzle` | `swizzle` zeroes out-of-range lanes; `shuffle` cannot have them |
| **SIMD 9** | `i8x16.shuffle` | Five arguments (two inputs, two selector halves, out); a single 16-byte selector immediate is not a LibJIT primitive |
| **remaining work** | 1,000,000-link tails, shared reference atomics, block params, bulk memory64, catch payloads, LibJIT frame | “Measure the set, not the count”: equal assertion counts are only meaningful when every case runs |
## 5. The defect log

Root causes, not just symptoms. A defect marked **open** is in
[`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8.

| ID | Defect | Root cause | Fix / evidence |
|---|---|---|---|
| `D-L1` | LibJIT corrupts the sixth integer argument (`r9`) | libjit codegen, not the engine IR; `jit_dump_function` showed a correct IR and a correct register/stack split | recorded deviation; the opt-in frame form retires it |
| `D-1` | “LibJIT uncaught throw” | the **probe** asserted a trap where the frozen contract is a normal return with a pending exception | probe corrected; no engine change |
| `D-2` | `array.fill` aborts on AsmJIT and SLJIT | both used the fill **value** as the offset | both lowerings and the value-first parity fixture fixed; LibJIT was already conformant |
| `D-3` | SLJIT `struct.get` returns garbage from a reference field | every field was loaded as 4 bytes | load and slot width follow the field type |
| `V-1` | a `try_table` catch disabled later stack checks | `dispatchTryTable` set `typeTainted` on any catch | payload is checked at the instruction; regression fixture `invalid_catch_stack.wat` |
| AsmJIT `narrow` | packed narrowing produced wrong lanes | the quarantined implementation was never pinned as correct | SIMD slice 5; golden lane vector |
| Trap leak | a trap abandoned the frame’s reference locals | the zero-return epilogue and explicit `unreachable` skipped the release | every trap path releases; `trap_after_alloc` |
| Mixed graph | a register-form caller into a frame-form callee crashed | the fixture called a frame export through the raw register entry; AsmJIT’s call lowering also used the caller’s form | fixture corrected; `dispatchCall` uses the callee’s form; `call_form_provider/importer.wat` |
| SLJIT opt-in | `setFrameCallAbiOptIn` was a no-op on SLJIT | it derived the form from signatures alone | the opt-in decides the module form |
| `table.set` | the reference slot’s stack count leaked on SLJIT/AsmJIT | no release on the barrier | same barrier as the other reference stores |
| AsmJIT packed default | `struct.new_default` refused packed fields | the lowering treated the whole shape as unsupported | packed field written; `[staging][abi][gc]` |
| `throw_ref` | the operand was typed as the bare `ExnRef` code | a `(ref null exn)` is a `Ref`/`RefNull` with the exn heap type | validator uses `refSubtype(..., ExnRef)` |
| `moduleNeedsFrame` | a synthetic multi-result **block** type switched the whole AsmJIT module to the frame ABI | it scanned the whole type section | scan only import and internal function signatures |
| `dispatchUnreachable` | returned a 32-bit zero for an i64 function | the placeholder ignored `resultTypes` and the frame/vector buffer | follows the result kind, frame mode and vector-buffer case |
| Native-import reference result | the caller takes over a count the host still owns | only AsmJIT clones a native import’s reference result | all three clone it — AsmJIT `retainNativeCallResult`, SLJIT `emitRetainNativeCallResult`, LibJIT `resolveCallReferences` — and `native_ref_result_probe.wat` asserts the host’s count; `GAP-5` **closed 2026-11-11** |
| Continuation kind count | `wasm_cont_suspend_ex` passed the *output capacity* where `AWorker::suspend` copies that many *kind* bytes, so a one-byte kind vector was read sixteen bytes deep | the caller confused “how many results may I write” with “how many kinds do I carry” | bound by the tag’s declared result list, capped by the frame capacity — found by the first SLJIT ASan run (`GAP-14`, closed 2026-11-11) |
| SLJIT continuation leak | 10,568 bytes in 17 allocations from `wasm_cont_switch` / `wasm_cont_resume_ex` / `invokeFfiContinuation`; the same cases are clean on LibJIT | the SLJIT lowering abandons a handle the guest drops elsewhere | **open** as `GAP-18`; reproduce with the SLJIT ASan run of `*continuation*` |
| `ref_test` on an `externref` | `ref_test_extern` answers `0` where the spec says `2` on SLJIT and AsmJIT; LibJIT passes | `ref.test` over an `externref` is lowered with the wrong heap-type test on both newer backends | **open** as `GAP-15`; the `[spec]` runs that `GAP-9` asked for found it (2026-11-11) |
| AsmJIT strict-sweep crash | the full `WASM_SPEC_VALIDATE=1` run ends in `SIGSEGV` in `spec: table_copy` at case 392, while the case passes alone | not yet diagnosed; order- or state-dependent | **open** as `GAP-16` (2026-11-11) |
| Plugin install rule | a regenerated `LibJit.pro`-derived Makefile installed no `libwasmjit-backend-libjit.so`, so `install-check.sh` failed at ES-5 | `plugin.files` pointed at a symlink the *post-link* creates, which qmake does not have at evaluation time | `plugin.CONFIG += no_check_exist` (`GAP-17`, closed 2026-11-11) |
| `sljit_get_local_base` | the frame migration asserted inside SLJIT | `dstw` must be zero; the offset was passed in that position | one transposed argument, fixed |
| AsmJIT frame epilogue | a frame function stored a vector result on the trap path | `bufferedVectorResult` won over `frameMode` in a branch-order bug | explicit frame branch; results undefined on a trap |

## 6. Reference ownership and the call form

The ownership model is in [`../WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) §6.
The campaign closed the consumer sites one by one: `ref.test`/`ref.is_null`/
`ref.eq`, `ref.cast`/`ref.as_non_null`, packed `struct.get_s/u`/`array.get_s/u`,
call arguments, a native import’s borrowed result (AsmJIT only, see `GAP-5`),
`table.set`, and every trap path. The fixture is `gc_ownership.wat`; the exact
count of every object is asserted after each shape.

Two limits remain and are registered, not hidden:

- a reference read out of a struct field or array element is a **borrow**;
  dropping it does not release a count (`GAP-4`);
- a reference-returning native import is cloned only on AsmJIT (`GAP-5`).

The call-form work exposed that the form is **module-wide**. A mixed graph now
works because a direct call emits the callee’s form, but one wide function still
puts every call in its module on the frame path. That cost is `GAP-1`; the
current contract and the recommendation are in [`../ABI.md`](../ABI.md) “Call
forms”.
## 7. Process lessons worth carrying forward

### 7.1 Probes and tests

- **A probe records one of three states** — works, refuses with a reason, or
  silently answers wrong. Only the third is a defect; refusal is a pass.
- **Assert the property, not the outcome.** A probe that asserts “it works”
  cannot tell a working backend from one that happens to return 0.
- **Test data must be inert.** A fabricated pointer as an `externref` was
  retained in host-handle bookkeeping and broke an unrelated test; use `nullptr`.
- **Validate the fixture before believing it.** Run `wasm-tools validate` with
  the proposal flags. The engine accepting a module is not proof that it is valid.
- **Two backends agreeing is not a spec check.** A parity fixture can encode the
  same misreading; compare with the validator and the upstream `.wast`.
- **A filtered green run is not a green tree**, and a test run in parallel with
  its build reads the old library. Build to a log, then run.
- **A pipe can kill a build.** `make … | head` dies of `SIGPIPE` and leaves stale
  `.so` files; compare source and library timestamps.
- **Measure the set, not the count.** Equal assertion counts matter only when
  every case runs on every backend.

### 7.2 Debugging and design

- **Grep before writing a guard.** Before claiming a path is untested or broken,
  run the test that would prove either answer. Three “missing features” in this
  record already existed.
- **A validator taint is a silent acceptance.** Prefer modelling the rule; a
  global `typeTainted` hides every later mismatch, including real ones.
- **Dump instead of guess.** `jit_dump_function` turned a week of hypotheses into
  one read.
- **Refusal beats corruption**, and beats aborting.
- **No partial modules:** a backend that cannot compile a function throws at
  module level, naming the function and the reason. An exported function must
  never be one that cannot be called.
- **The measuring pass is part of a lowering.** SLJIT’s first atomic helpers
  updated the stack model only in the emit pass; the measure pass underflowed.
- **Header docs are enforced:** `doxygen` caught a missing `\param` immediately;
  `[headerdoc]` keeps the shape.

## 8. References

- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8 — the current open
  register; §13 — the current backend verdict.
- [`../ABI.md`](../ABI.md) — the canonical signature and the two call forms.
- [`../ABI.md`](../ABI.md) — the target matrix and the
  GC object ABI.
- [`../WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) §6 — reference ownership;
  §11 — the SLJIT worked example.
- [`../TESTING.md`](../TESTING.md) — the suite, the probe recipe and the
  promotion gate.
- [`../../Test/abi/ABI_BASELINES.md`](../../Test/abi/ABI_BASELINES.md) — the
  link-visible ABI baselines.
- Git history — the deleted per-slice reports, verbatim, for any detail this
  compressed record omits.
