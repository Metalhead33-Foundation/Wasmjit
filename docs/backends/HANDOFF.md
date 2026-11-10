# Backend handoff {#backend_handoff}

*State of the tree for the next session*

**Status:** handoff record, revision 2 (2026-11-10). It is kept as the list of
sweep defects and open work from the 2026-11-07 session; the **current** verdict
is owned by [`BACKEND_PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md)
and the current per-feature ledger by [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md)
§27/§30. The snapshot below was re-measured on 2026-11-10.

**Written:** 2026-11-07, `backendsMh33`, after the defect round, the capability
close-out and the validator/ multi-value follow-up.
**Read first:** [`BACKEND_PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md) (the
production verdict), [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) section 27 (the
curated capability ledger) and section 29 (the production gaps), and
[`CAPABILITY_PROBES.md`](CAPABILITY_PROBES.md) section 19 (the latest re-run).

## 1. Where the tree is

**Snapshot (2026-11-10, working tree on `2cce7b9`):** the generic
backend-selected suite is **green on all three code generators**, with the same
case set and assertion count each:

```
WASMJIT_BACKEND=libjit ./WasmJit --rng-seed 12345 -> 22,593 assertions in 824 cases
WASMJIT_BACKEND=sljit  ./WasmJit --rng-seed 12345 -> 22,593 assertions in 824 cases
WASMJIT_BACKEND=asmjit ./WasmJit --rng-seed 12345 -> 22,593 assertions in 824 cases
```

Gates: `[headerdoc]`, `[docs]`, `doxygen` (0 warnings), the strict
`WASM_SPEC_VALIDATE=1` sweep on SLJIT (curated and `[spec]`), and the ASan
configuration.

All three backends are now production-ready within the same declared envelope
(x86-64 Linux / SysV AMD64); the verdict and the documented refusals live in
[`BACKEND_PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md) §7, and
the defect ledger that closed the last failures is
[`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) §30. The LibJIT frame form stays
opt-in and its frame tails are call+return rather than constant-stack.

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
| direct self-recursive `return_call` in constant stack | OK | OK | OK | `TAILSELF` 1,000,000 links, `run()=0` |
| structured block/loop/if parameters | OK | OK | OK | `BLOCKPARAM` block=42 if=11/12 loop=0; `BLOCKV128` block=42 if=7/8 loop=7 |
| direct cross-function `return_call` (mutual) | OK | OK | OK | `TAILMUTUAL` 1,000,000 links, `run()=1` |
| indirect cross-function `return_call_indirect` | OK | OK | OK | `TAILINDIRECT` 1,000,000 links, `run()=1` |
| frame-form cross-function `return_call` (scalar) | OK | OK | OK | `TAILFRAME` 1,000,000 links, `run()=1` |
| frame-form cross-function `return_call` (v128) | n/a | OK | OK | `TAILV128FRAME` 1,000,000 links, `run()=1`; LibJIT native trampoline excludes v128 |
| `call_ref` / `return_call_ref` (register form) | OK | OK | OK | `TAILREF` 1,000,000 links, `even0=1 run()=1` |
| frame-form `call_ref` / `return_call_ref` | OK | OK | OK | `TAILREFFRAME` 1,000,000 links, `run()=1` |
| v128 frame-form `return_call_ref` | n/a | OK | OK | `TAILV128REFFRAME` 1,000,000 links, `run()=1`; LibJIT native trampoline excludes v128 |
| `table.get` / `table.set` (externref/funcref, OOB trap) | OK | OK | OK | `TABLEACCESS` ext=1 fun=0 |
| shared reference atomics (struct/array) | OK | OK | OK | `SHAREDREF` run=1 array=1 |
| shared table atomics (anyref/eqref) | OK | OK | OK | `TABLEATOMIC` run=1 |
| global reference atomics (eqref) | OK | OK | OK | `GLOBALREF` run=1 |
| v128 atomic slots rejected at validation | OK | OK | OK | `V128ATOMIC` rejected (global/struct/array) |
| structured `v128` block/loop/if results | OK | OK | OK | `BLOCKRESV128` block=5 branch=6 if=7/8 loop=9 |
| multi-result `call_ref` / `return_call_ref` | OK | OK | OK (frame opt-in) | `TAILREFMULTI` run()=11; LibJIT frame probe run()=11 |
| frame-form indirect tail calls | OK | OK | OK | `TAILINDIRECTFRAME` 1,000,000 links, `run()=1` |
| catch payload to a loop parameter | OK | OK | OK | `CATCHLOOP` run()=42 |
| bulk memory on memory64 | OK | OK | OK | `MEM64BULK` copy=97 fill=65 init=97 |
| LibJIT frame calls / native imports / multi-result | n/a | n/a | OK | frame call 6, import 42, multi-result 7/9, frame tail 1, multi-result `call_ref` 11 |

### Remaining (honest refusals)

| Gap | Backends | Reason |
|---|---|---|
| Full generic-suite green | SLJIT, AsmJIT | SLJIT: 41 failures then abort; AsmJIT: 6 failures then segfault. Detailed verdict: [`PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md). |
| *(none in the curated capability ledger)* | -- | The LibJIT frame form remains opt-in; that is policy, not a missing capability. |

`v128` atomic slots are **not** on this list: the atomic proposal does not allow
them, and the validator now rejects them (`V128ATOMIC`).

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

**State at the end of the 2026-11-09 pass.** Reference ownership is exact on all
three backends, measured shape by shape: 22,593 assertions in 824 cases on **all
three**, identical. That equality now covers the last capability difference too --
AsmJIT calling a compiled callee that was built in the other call form. The consumer sites that were
"left for later" are closed -- `ref.test`/`ref.is_null`/`ref.eq`, the pass-through
casts, the packed field and element reads, call arguments, a native import's
borrowed result, and every trap path. `PRODUCTION_GAPS.md` §32 carries the table.

What is left, in the order it should be taken:

1. **Per-signature call form.** The one remaining *design* gap: the form is still
   decided per module, so one wide function makes every call in that module pay the
   frame's stores and loads and forfeits the register tail-call trampoline
   module-wide. [`CALL_FORM_GRANULARITY.md`](CALL_FORM_GRANULARITY.md) has the rule,
   the sites it touches, and the contract question that makes it a milestone (a
   per-signature form makes `frameCallAbi()` -- "every export of this module is
   frame-form" -- false, and 63 call sites rely on it). Its section 8 records the
   agreed direction instead: make the frame form the *default* on SLJIT and
   AsmJIT, whose register form is the source of the ceiling, of the mixed-graph
   case and of the tail-call fallback, and keep the register form on LibJIT. The
   obstacle is named there too: for a tail-capable module `Callable::fnPtr` is the
   driver, so marshalling in `callCallable` calls a register-form driver with a
   frame -- the driver has to present the frame shape at the boundary first.
2. **`return_call_ref` above the register-form ceiling** is a semantic call plus
   return on SLJIT ([`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) section 2.1). In a
   frame-form module the frame trampoline carries the chain, so this closes with
   item 1 rather than on its own.
3. **The atomic table `rmw` forms** (`xchg`, `cmpxchg`) have no fixture: a shared,
   reference-typed table is not constructible in the harness yet. `table.set`,
   `table.get` and `table.atomic.set` are fixed and measured.
4. **Portability.** Every production sentence in this tree is scoped to x86-64
   Linux / SysV AMD64. A single non-SysV runner (AArch64 or MS x64) would turn
   the `provisional` rows in [`ABI_PORTABILITY.md`](../ABI_PORTABILITY.md) into
   evidence.

*Answered by analysis, no code change:* the staged-import cost on SLJIT. Its
C-call form takes four argument entries and `VMContext*` is the first, so
`ctx + 3` is the maximum and reserving `S0`/`S1` cannot express a wider signature.
Staging is the only shape that can, so the cost is structural rather than a
missing optimisation.

Keep the probe discipline: strengthen the fixture until it observes the
Keep the probe discipline: strengthen the fixture until it observes the
property, validate the module, run all three backends, then record a row.
