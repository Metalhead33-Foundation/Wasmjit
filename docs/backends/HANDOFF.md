# Handoff: state of the tree for the next session

**Written:** 2026-11-07, `backendsMh33`, after the defect round, the capability
close-out and the validator/ multi-value follow-up.
**Read first:** [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) section 27 (the current
ledger; 23-26 are history) and [`CAPABILITY_PROBES.md`](CAPABILITY_PROBES.md)
section 18 (the latest re-run).

## 1. Where the tree is

**Snapshot (2026-11-07, this working tree):** `--rng-seed 12345` -> **22122
assertions, 795 cases, green**. Gates: `[headerdoc]`, `[docs]`, `doxygen` 0
warnings, `abi-check` OK.

The frame ABI work is done on all three backends. Exceptions (including
`catch_ref` and multi-value payloads), the memory-atomics family, the numeric
shared-everything atomics and frame-form tail calls are implemented on SLJIT and
AsmJIT. The remaining rows are listed in section 3.

## 2. The sweep defects

* **D-1** (LibJIT uncaught throw) was **not a backend defect**: the probe asserted
a trap while the frozen contract is a normal return with a pending exception.
The probe was corrected; no LibJIT code changed.
* **D-2** (LibJIT `array.fill` abort) was **real on AsmJIT and SLJIT**: both used
the fill value as the offset. Both lowerings and the value-first parity fixture
were fixed; LibJIT was already conformant and the abort did not reproduce.
* **D-3** (SLJIT reference struct field) was real: `struct.get` loaded every
field as 4 bytes. The load and the slot width now follow the field type.

## 3. Capability gaps: closed and remaining

### Closed

| Capability | SLJIT | AsmJIT | LibJIT | Evidence |
|---|---|---|---|---|
| `throw`, `throw_ref`, `try_table`, `catch`, `catch_all` | OK | OK | OK | local catch 42, cross-instance 7, uncaught pending |
| `catch_ref` / `catch_all_ref` and multi-value catch payloads | OK | OK | OK | `catch2=22 catch2i32=33 catchref=33` |
| multi-value structured blocks | OK | OK | OK | `block2=33 direct=33` |
| memory atomics: load/store/RMW/cmpxchg, fence, pause, notify, wait | OK | OK | OK | `i32=9 i64=10 i8=0 i16=0`, `bump=0,1` |
| **numeric** shared-everything atomics (global/struct/array) | OK | OK | OK | `global=5 struct32=7 struct64=9 array=11` |
| frame-form `return_call` / `return_call_indirect` | OK | OK | OK | `tail_wide=1` |

### Remaining (honest refusals)

| Gap | Backends | Reason |
|---|---|---|
| constant-stack tail-call trampolines | SLJIT, AsmJIT | `return_call` is still call + return; the values are correct, the host stack grows |
| shared-everything atomics on **reference** slots and **tables** | SLJIT, AsmJIT | numeric slots are done; a reference slot needs the GC barrier, and table get/set is itself unimplemented on these backends |
| structured **block parameters** | AsmJIT | block results (including multi-value) work; parameters are still refused |
| bulk memory on memory64 | SLJIT, AsmJIT | message says "yet" |
| LibJIT frame-mode calls / imports / multi-result | LibJIT | **policy**: the frame form is opt-in and partial on purpose |

## 4. Findings from this round (record before editing)

1. **V-1 (fixed):** `Validator::dispatchTryTable` used to set `typeTainted`
   whenever a catch existed, which disabled every later stack check. A module
   with a catch could leave extra values at any block end and still validate.
   The catch payload is already checked against the target label at the
   `try_table` instruction, so the taint was removed. A regression fixture
   (`invalid_catch_stack.wat`) now requires the rejection.
2. **The taint was hiding invalid hand-written fixtures.** `exceptions.wat`,
   `native_import_exceptions.wat`, `cont_throw.wat` and `cont_throw_handler.wat`
   all left a value on the stack or declared the wrong try-table result. They
   are corrected and validate with `wasm-tools validate` (plus the proposal
   feature flags).
3. **`throw_ref` typed its operand as the bare `ExnRef` code.** A `(ref null
   exn)` is a `Ref`/`RefNull` with the exn heap type; the validator now uses
   `refSubtype(..., ExnRef)`. This is what let `spec: throw_ref` pass again after
   V-1.
4. **`TypeTranslator::moduleNeedsFrame` scanned the whole type section.** A
   synthetic multi-result *block* type made AsmJIT switch the entire module to
   the frame ABI, so callers using the register form read the frame pointer as
   the result. It now scans only import and internal function signatures.
5. **AsmJIT's `dispatchUnreachable` returned a 32-bit zero for an i64 function**
   (and a register for a frame signature). It now follows `resultTypes`, frame
   mode and the vector-buffer case.

## 5. Process hazards this campaign paid for

1. **Validate the fixture before believing it.** Run `wasm-tools validate` on
   every WAT (with the proposal feature flags for shared/stack-switching). The
   engine accepting a module is not evidence that it is valid.
2. **Two backends agreeing is not a spec check.** A parity fixture can encode
   the same misreading; compare with the validator and the upstream `.wast`.
3. **A filtered green run is not a green tree**, and a **test run in parallel
   with its build** reads the old library. Build to a log, then run.
4. **A pipe can kill a build.** `make ... | head` dies of `SIGPIPE` and leaves
   stale `.so` files; compare source and library timestamps.
5. **The measuring pass is part of a lowering.** SLJIT's first atomic helpers
   updated the stack model only in the emit pass; the measure pass underflowed.
6. **A validator taint is a silent acceptance.** Prefer modelling the rule; a
   global `typeTainted` hides every later mismatch, including real ones.
7. **Mechanical:** the editor tool doubles `\` in Doxygen commands. After a
   header edit run `doxygen`; `[headerdoc]` enforces the shape.

## 6. Test-organisation rules in force

* **No partial modules** -- a backend that cannot compile a function throws at
  module level, naming the function and the reason.
* **Assert the property**, not the outcome.
* **Refusal is a pass** in a probe; a runtime trap on an in-bounds operation is
  a defect.
* **Pin nothing.** A known wrong answer is recorded as a defect, never asserted
  as the expected result.

## 7. Next session's recommendation

1. **True tail-call trampolines.** This is the one large architectural item left:
   a driver per function and a uniform thunk per type (LibJIT's model), or a
   libffi closure driver. Portability and the ABI contract outrank speed; do not
   claim the space guarantee until a deep `return_call` chain runs in constant
   native stack on the backend.
2. **Shared reference/table atomics.** Numeric slots are done. Reference slots
   need the retain/release barrier (`wasm_atomic_ref_*`); table slots additionally
   need table.get/set lowering, which neither backend has yet.
3. **AsmJIT block parameters.** Multi-value block *results* work now; a block
   with parameters still refuses. The frame's entry height must subtract the
   parameter count, and branch/end transfers must account for them.
4. **Bulk memory on memory64** on SLJIT and AsmJIT.

Keep the probe discipline: strengthen the fixture until it observes the
property, validate the module, run all three backends, then record a row.
