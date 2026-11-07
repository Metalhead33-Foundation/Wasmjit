# Handoff: state of the tree for the next session

**Written:** 2026-11-07, `backendsMh33`, after the defect round and the capability
close-out that followed the probe sweep (probes 1-21).
**Read first:** [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) section 26 (the current
ledger; sections 23-25 are the superseded history) and
[`CAPABILITY_PROBES.md`](CAPABILITY_PROBES.md) section 17 (the re-run instruments).

## 1. Where the tree is

**Snapshot (2026-11-07, this working tree):** `--rng-seed 12345` -> **22058 assertions,
792 cases, green**. Gates: `[headerdoc]`, `[docs]`, `doxygen` 0 warnings, `abi-check` OK.

The frame ABI work is **done and tested on all three backends**. Exceptions, the
memory-atomics family and frame-form tail calls are now implemented on SLJIT and
AsmJIT as well; the remaining rows are listed in section 3.

## 2. The three sweep defects: two fixed, one reclassified

### D-1 -- LibJIT uncaught `throw` -- **not a backend defect**

The sweep recorded "an uncaught `throw` returns normally" as a defect. The frozen
contract ([`docs/ABI.md`](../ABI.md) "Exceptions", `EXCEPTIONS.md` EXD1) defines an
uncaught throw as a **normal return with a pending exception**, and `Test/main.cpp`
always asserted it. The probe was asserting a trap. It now checks the documented
property (zero result, pending payload, tag id); no LibJIT code changed.

### D-2 -- REAL, and it was AsmJIT and SLJIT: `array.fill` offset/value swapped

Operand order is `[array, offset, value, count]` (validator and upstream
`array_fill.wast`). Both backends used the value as the offset. Their shared parity
fixture was written value-first, so the two wrong backends agreed and both tests were
green. Both lowerings and the fixture are fixed; the parity test now includes LibJIT.
The LibJIT `popValueRaw` abort did not reproduce with the committed fixture.

### D-3 -- REAL and fixed: SLJIT `struct.get` loaded every field as 4 bytes

`dispatchStructGet` computed the width and then used `SLJIT_MOV32` for every field,
leaving the upper half of a reference slot stale. The load and the slot width now
follow the field type; packed and `v128` fields are refused explicitly.

## 3. Capability gaps: what closed, what remains

### Closed this session

| Capability | SLJIT | AsmJIT | LibJIT | Notes |
|---|---|---|---|---|
| `throw`, `throw_ref`, `try_table`, `catch`, `catch_all` | ✅ | ✅ | ✅ | static/runtime dispatch, payload layout, pending protocol, cross-instance payloads |
| memory atomics: load/store/RMW/cmpxchg, fence, pause, notify, wait | ✅ | ✅ | ✅ | all widths and RMW operations, through the shared `wasm_atomic_*` ABI |
| frame-form `return_call`, `return_call_indirect` | ✅ | ✅ | ✅ (register form) | correct values; implemented as call + frame return |

Probe outputs (`[exceptions]`, `[atomics]`, `[tail]`):

```
EXCUNCAUGHT libjit/asmjit/sljit: returned=0 pending=set
EXCH        libjit/asmjit/sljit: caught()=42
XINST       libjit/asmjit/sljit: caught()=7
ATOMICSOPS  libjit/asmjit/sljit: i32=9 i64=10 i8=0 i16=0
TAILFRAME   asmjit/sljit: tail_wide=1
```

### Remaining (honest refusals)

| Gap | Backends | Note |
|---|---|---|
| `catch_ref` / `catch_all_ref` and a catch with more than one payload value | AsmJIT | a structured block on AsmJIT carries at most one result |
| shared-everything atomics (global/table/struct/array) | SLJIT, AsmJIT | memory atomics work; these still refuse |
| a true constant-stack tail-call trampoline | SLJIT, AsmJIT | `return_call` is call + return; the values are correct, the host stack still grows |
| bulk memory on memory64 | SLJIT, AsmJIT | message says "yet" |
| LibJIT frame-mode calls / imports / multi-result | LibJIT | **policy**: frame mode is opt-in and partial on purpose |

## 4. Findings from this round (record before editing)

1. **The validator accepted two invalid fixtures.** `exc_handler_probe` and
   `tag_consumer` left a value on the stack at the end of a block; `wasm-tools
   validate` rejects both, while the engine's validator accepted them (LibJIT and
   SLJIT then executed the catch path, hiding it). The fixtures are corrected. The
   validator gap itself is recorded as **V-1** in `PRODUCTION_GAPS.md` section 26 and
   is not fixed here.
2. **A dead function end in frame mode aborted SLJIT.** `dispatchEnd`'s unreachable
   epilogue emitted a typed return for a `void(ctx, frame)` signature. It now returns
   void in frame mode.
3. **AsmJIT's frame-mode float return was wrong.** `compile()` tested `isFloatKind`
   before `frameMode`, so a float result returned in an Xmm from a void signature.
   The frame branches now come first; `emitFrameReturnValue` is shared by the normal
   end, an explicit `return` and `return_call`.

## 5. Process hazards this campaign paid for (read before editing)

1. **Grep for the guard before writing it.** Three times a "missing" guard already
   existed (`kMaxWasmParameters`, SLJIT's multi-result check, the exit to
   `unsupportedReason`).
2. **A sentinel means what the tree says.** A null `fnPtr` means both "unsupported"
   and "no compiled entries".
3. **Instrument before theorising.** Print; one probe settles what two edit rounds
   cannot.
4. **A filtered green run is not a green tree.** Run `--rng-seed 12345` over the whole
   suite.
5. **Validate the fixture before believing it.** Four probes measured themselves this
   campaign (invalid `ref.func`, missing `array.fill` count, a `get` on a consumed
   array, and the two block-end fixtures above). `wasm-tools validate` every WAT
   before recording a row.
6. **Two backends agreeing is not a spec check.** `gc_array_fill` passed on AsmJIT and
   SLJIT because both encoded the same transposition. Compare with the validator and
   the upstream `.wast`, and keep LibJIT in a parity test.
7. **A closed pipe can kill a build.** `make ... | head` makes GNU make die of
   `SIGPIPE` part-way through the subdirectories, leaving stale `.so` files. Build to
   a log file, and compare source and library timestamps when a fix "does nothing".
8. **Measure-pass bookkeeping is part of a lowering.** The first SLJIT atomic helpers
   pushed their result only in the emit pass; the measuring pass then underflowed. A
   new operation must update the model in both passes.
9. **Mechanical:** the editor tool doubles `\` in Doxygen commands. After a header
   edit, run `doxygen` and expect to reorder merged doc blocks.

## 6. Test-organisation rules in force

* **No partial modules** -- a backend that cannot compile a function throws at module
  level, naming the function and the reason.
* **Assert the property**, not the outcome (round-trip identity, not "it returned").
* **Refusal is a pass** in a probe; a runtime trap on an in-bounds operation is a
  defect.
* **Pin nothing.** A known wrong answer is recorded as a defect, never asserted as the
  expected result.

## 7. Next session's recommendation

1. **True tail-call trampolines for SLJIT and AsmJIT.** The frame pointer is the
   missing sixth trampoline argument on SLJIT; AsmJIT's register-form thunk currently
   supports only i32. Until then, `return_call` is correct but not constant-stack.
2. **`catch_ref` / multi-payload catches on AsmJIT.** The block multi-value limit is
   the blocker; either lift it or route such catches through a frame slot.
3. **Shared-everything atomics on SLJIT and AsmJIT.** The memory family is done and
   the address-forming code exists for the GC paths; the global/table forms need the
   same helper with the GC write barrier (`wasm_atomic_ref_*`).
4. **V-1: the validator's block-end stack check.** `wasm-tools validate` rejects a
   module the engine accepts. This is a core validator gap, independent of any
   backend, and it hid two invalid probe fixtures.

Keep the probe discipline: strengthen the fixture until it observes the property, run
it on all three backends, validate the module with `wasm-tools`, and only then record a
row.
