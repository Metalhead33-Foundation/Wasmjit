# Post-mortem: the AsmJit/SLJIT P0–P2 parity slice

**Snapshot (2026-11-05, working tree on `backendsMh33`):** one slice brought
`Sljit/` up to `Asmjit/`'s execution surface, registered AsmJit under its name,
and added a cross-backend conformance test over one fixture set. It is written
for the fourth backend author, not as a changelog.

## 1. What the shared ABI forced

- **The callable signature is `ret fn(VMContext*, params...)`.** Both backends
  reproduce it by construction; SLJIT's prologue copies parameters out of the
  saved argument registers into frame slots so that its slot-based operand
  model never depends on register pressure.
- **A trap is a pending status, not a non-local jump.** Every trapping
  operation in SLJIT (division, memory bounds, indirect-call checks, bulk
  operations) calls a C helper, stores the helper's result, probes the pending
  slot and branches to one shared epilogue. The epilogue annotates the function
  and returns a benign value; `docs/SAFETY.md` §2.4 is why a C++ exception never
  unwinds a generated frame.
- **A native import passes the caller's context; a Wasm import passes its own.**
  The resolver returns the `Callable*`; generated code loads `fnPtr` and
  `context` at the documented offsets and selects the caller context when the
  field is null. This is the same rule AsmJit implements, now exercised by the
  shared fixtures rather than by prose.
- **Unload must give the code back.** SLJIT frees every entry point in
  `releaseInstance`; AsmJit releases the module's single installed allocation.
  Both are idempotent and clear `fnPtr`/`rawFnPtr`/`tailEntry`.

## 2. What broke, with the failing fixture

1. **`call_indirect` passed the wrong slots.** The first implementation computed
   the argument base as `stack.size() - params`, which is the selector slot, not
   the first parameter, because the selector sits above the arguments. The
   observable failure was `call_indirect(21, 0) == 21` (the callee never saw the
   intended argument pattern) in
   `Test/SljitBackendTests.cpp` (`sljit: the shared integer module executes`).
   Fix: consume the selector before the common call path computes its base.
2. **The refusal test used an opcode the backend now supports.**
   `i32.div_s` was the old "unsupported" fixture; it is now a checked helper.
   The test was moved to `f32.add`, which is still outside the slice.
   (The first byte encoding of that fixture also had a wrong body size; the
   loader's "section size exceeds the input" was correct.)
3. **`requireTableIndex`/`blockSignature` were const but call `require`, which
   records a reason.** The constness was an interface bug, not a semantic one;
   they are non-const now.

## 3. What the toolkit could not express

| Constraint | Consequence | Accepted or temporary |
|---|---|---|
| `sljit_emit_enter`/`sljit_emit_icall` take at most four integer arguments | at most three Wasm parameters and one result | accepted for the slice; documented in the README and §11 |
| SLJIT has no cross-compiler call target | direct calls resolve through `ModuleCompiler::resolveCallable` at run time (one extra indirect call) | accepted; an inlining slice may remove it once measured |
| SLJIT has no `POPCNT` op | `i32.popcnt` uses a backend-private helper | accepted |
| helper calls carry at most four arguments | memory/table helpers pack two 32-bit operands into one 64-bit word (`(hi << 32) | lo`) | accepted; the packing is documented at the helper |
| one W^X mapping per function | `SLJIT_WX_EXECUTABLE_ALLOCATOR=1` trades density for no RWX pages | accepted |

## 4. What the tests did not catch

The per-backend suites duplicated expectations from the AsmJit suite, so a
fixture change could have left both green while the two engines disagreed. The
new `asmjit and sljit agree on the shared integer module` case instantiates the
same `Wasm::Module` with both compilers and compares *outcomes* (value or trap)
for 34 probes. It is now the gate for a shared-slice change.

## 5. What was slower, and by how much

**Not measured.** No benchmark was run in this slice, so no performance claim is
made. The expected cost is the runtime callable lookup plus a helper call per
trapping operation; the P6 slice must measure it before either backend is called
competitive.

## 6. What a fourth backend should copy, and what it must not

Copy:

- the slot-based operand model with an explicit width stack; it makes branch
  result transfer and `select` mechanical;
- the two-pass measure/emit structure (the frame is fixed by `sljit_emit_enter`);
- the single pending-trap epilogue and the "store the helper result first, then
  probe the slot" ordering;
- `releaseInstance` from day one, not after the unload test fails;
- a cross-backend conformance test over one `.wat` fixture set.

Do not copy:

- the runtime `resolveCallable` lookup if the toolkit can bind a forward call
  label (AsmJit can; SLJIT cannot);
- the four-argument assumption if the toolkit has a wider call ABI — document
  the limit rather than encoding it everywhere;
- duplicated expectation tables as the only parity evidence.

## 7. Residual refusals

| Opcode family | Refusal | Test |
|---|---|---|
| floating point | function left without `fnPtr` | `sljit: an unsupported opcode is refused, never miscompiled` (`f32.add`) |
| `v128`/SIMD | function left without `fnPtr` | same refusal path; `[sljit]` lane-wise probe covers only the session diagnostic |
| references/GC | function left without `fnPtr` | same refusal path |
| exceptions/continuations | function left without `fnPtr` | same refusal path |
| atomics | function left without `fnPtr` | same refusal path |
| memory64 | function left without `fnPtr` | same refusal path |
| >3 parameters / >1 result | function left without `fnPtr` | `kMaxWasmParameters` check; no fixture yet |

Packaging is not part of this slice: `libwasmjit-backend-*` and
`Test/install-check.sh` still cover the LibJIT backend, while the AsmJit and
SLJIT registrations are in-process. A plugin build for either is a P6 item
(§14.3); nothing in this slice changes the core's packaging surface.

## 8. Sign-off

- Slice: P0–P2 parity (registration, release, integer/control/call/memory32).
- Commit: working tree on `backendsMh33` (not committed in this session).
- Commands and counts: `./WasmJit '[sljit]'` — 505 assertions in 12 test cases;
  `./WasmJit '[asmjit]'` — 417 assertions in 19 test cases (the A/B case is
  tagged with both); the full suite — 19052 assertions in 667 test cases (the
  A/B case contributes 138 assertions); the shared-slice cases contribute 337
  assertions across four `[parity]` cases. (These are the counts at this
  slice's sign-off; the P3 slice added float cases and extra pre-call
  assertions, so the current totals are larger.)
- Doxygen: exit 0, no warnings.
