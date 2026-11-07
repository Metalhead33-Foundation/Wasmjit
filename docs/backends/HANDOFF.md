# Handoff: state of the tree for the next session

**Written:** 2026-11-07, `backendsMh33`, after the defect round that followed the
capability sweep (probes 1-21).
**Read first:** [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) section 25 (the current ledger;
sections 23-24 are the superseded history) and [`CAPABILITY_PROBES.md`](CAPABILITY_PROBES.md)
section 16 (the corrected probes).

## 1. Where the tree is

**Snapshot (2026-11-07, this working tree):** `--rng-seed 12345` -> **22012 assertions,
791 cases, green**. Gates: `[headerdoc]`, `[docs]`, `doxygen` 0 warnings, `abi-check` OK.

The frame ABI work is **done and tested on all three backends**: one typed spelling in
both directions, frames invisible above the call cache, no partial modules anywhere,
and every unsafe shape refused with a reason.

## 2. The three defects: two fixed, one reclassified

### D-1 -- LibJIT uncaught `throw` -- **not a backend defect**

The sweep recorded "an uncaught `throw` returns normally" as a defect and asserted in the
`[exceptions]` probe that a throw "must never return normally". That contradicts the frozen
contract: [`docs/ABI.md`](../ABI.md) ("Exceptions") and
[`docs/proposals/EXCEPTIONS.md`](../proposals/EXCEPTIONS.md) EXD1 define an uncaught throw
as a **normal return with the exception left in the execution state's pending slot**, and
`exceptions: a caught throw carries its payload and an uncaught one stays pending` in
`Test/main.cpp` has always asserted exactly that.

The probe measured its own premise. It now asserts the documented property: it clears the
slot, calls `do_throw(7)`, requires the zero result (EXD1's "return zeroes"), takes the
pending payload, and checks its tag id against the module's tag index space.

**Nothing in LibJIT's exception path changed.** Probe 13 (handlers) and probe 21
(cross-instance payloads) still stand.

### D-2 -- REAL, and it was AsmJIT and SLJIT: `array.fill` offset/value swapped

The sweep recorded "LibJIT `array.set` / `array.fill` abort in `popValueRaw`". That abort
**did not reproduce** with the committed fixture on the current tree (assertions are active
in the Release build too). What the strengthened probe found instead is a real wrong answer
on the other two backends:

| | recorded | measured after strengthening the fixture |
|---|---|---|
| LibJIT | aborts | `set_get=9`, `fill_get=5`, `inline=0` -- correct |
| AsmJIT | "inconclusive: out of bounds" | trapped on an **in-bounds** fill -> offset/value swapped |
| SLJIT | "inconclusive: out of bounds" | the same swap |

Operand order is `[array, offset, value, count]` (validator in `WasmValidator.cpp` and the
upstream `array_fill.wast`). AsmJIT popped the value as the offset; SLJIT named the depth
the same way. Their shared parity fixture (`gc_array_fill.wat`) was written *value-first*,
so the two wrong backends agreed with each other and both parity tests passed. LibJIT and
the validator expected the spec order, which is why the same fixture trapped there.

**Fixed:** both lowerings use the spec order, the fixture is corrected, and the parity test
now instantiates **all three** backends from the corrected fixture. The probe was
strengthened from "execute and return 0" to `set_get` / `fill_get` / `inline`, so it
observes the mutation instead of the instantiation.

### D-3 -- REAL and fixed: SLJIT `struct.get` loaded every field as 4 bytes

`SljitOpcodeDispatcher::dispatchStructGet` computed the field width and then hard-coded
`SLJIT_MOV32` and `storeSlot(..., 4)`. For a reference field the low half of the pointer
was loaded and the slot's upper half kept whatever it held before -- hence
`roundtrip(null)=0x7f1700000000`. The load op and the slot width now follow the field type
(4 for `i32`/`f32`, 8 otherwise); packed and `v128` fields are refused explicitly, as
elsewhere in the backend.

The `REFFIELD` probe now requires `nullptr` for **every** backend instead of pinning the
old wrong answer.

## 3. Feature gaps (honest refusals, all temporary)

| Gap | Backends | Note |
|---|---|---|
| `throw`, `try_table` | SLJIT, AsmJIT | refused by dispatch; LibJIT implements the pending protocol |
| atomics | SLJIT, AsmJIT | consistent refusal across `rmw.add`, `store`, `load` |
| frame-form tail calls | SLJIT, AsmJIT | SLJIT needs a frame-carrying trampoline (6th thunk arg) |
| bulk memory on memory64 | SLJIT, AsmJIT | message says "yet" -- a scheduled limitation |
| LibJIT frame-mode calls / imports / multi-result | LibJIT | **policy**, not limitation: frame mode is opt-in and partial on purpose |

## 4. Process hazards this campaign paid for (read before editing)

1. **Grep for the guard before writing it.** Three times a "missing" guard already
   existed (`kMaxWasmParameters`, SLJIT's multi-result check, the exit to
   `unsupportedReason`).
2. **A sentinel means what the tree says.** A null `fnPtr` means both "unsupported" and
   "no compiled entries"; inferring support from it broke three tests.
3. **Instrument before theorising.** When two reverts agree, print; one probe settled what
   two edit rounds could not.
4. **A filtered green run is not a green tree.** Run `--rng-seed 12345` over the whole
   suite.
5. **Validate the fixture before believing it.** Three probes measured themselves (invalid
   `ref.func`, missing `array.fill` count, a `get` on a consumed array). This round added a
   fourth: a folded `array.fill` with offset and value transposed looked like a backend
   trap.
6. **Two backends agreeing is not a spec check.** `gc_array_fill` passed on AsmJIT and
   SLJIT because both encoded the same transposition. Compare against the validator and
   the upstream `.wast`, and always include LibJIT in a parity test.
7. **Do not project a known defect into untested territory** -- D-3 was nearly filed as
   "SLJIT mangles references" instead of "SLJIT mangles a reference in a struct field".
8. **A closed pipe can kill a build.** `make ... | head` makes GNU make die of `SIGPIPE`
   part-way through the subdirectories, leaving stale `.so` files that the next test run
   silently loads. Build to a log file (`> /tmp/build.log 2>&1`), and when a fix "does
   nothing", compare the source and library timestamps before theorising.
9. **Mechanical:** the editor tool doubles `\` in Doxygen commands, and inserting a
   `/*! */` block above a documented declaration makes Doxygen merge both docs onto one
   function. After any header edit: run `doxygen` and expect to reorder.

## 5. Test-organisation rules in force

* **No partial modules** -- any backend that cannot compile a function throws at module
  level, naming the function and the reason.
* **Assert the property**, not the outcome (round-trip identity, not "it returned").
* **Refusal is a pass** in a probe -- `continue` after printing, never `SUCCEED`.
* **A runtime trap on an in-bounds operation is a defect**, not a refusal and not a skip.
* **Pin nothing.** The two wrong answers this round were pinned by tests; both tests now
  assert the correct property, so a regression fails instead of matching.

## 6. Next session's recommendation

The defect ledger is empty. The remaining work is the capability matrix's refused rows, in
the order the project cares about:

1. **exceptions on SLJIT and AsmJIT** -- the LibJIT implementation is the worked example
   (static catch dispatch plus the pending-exception protocol);
2. **atomics on SLJIT and AsmJIT** -- `dispatchI32AtomicRmwAdd` and the store/load family;
3. **the frame-carrying trampoline** for tail calls (SLJIT first: the thunk already takes
   five arguments, so a sixth is the smallest shape).

Keep the probe discipline: strengthen the fixture until it observes the property, run it
on all three backends, and only then record a row.
