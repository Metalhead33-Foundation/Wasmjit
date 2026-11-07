# Handoff: state of the tree for the next session

**Written:** 2026-11-06, `backendsMh33`, after the capability sweep (19 probes, 13+ sections).
**Read first:** [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) (the ledger) and
[`CAPABILITY_PROBES.md`](CAPABILITY_PROBES.md) (the instruments).

## 1. Where the tree is

| | |
|---|---|
| Suite | `--rng-seed 12345` -> 21977 assertions, 790 cases, green |
| Gates | `[headerdoc]`, `[docs]`, `doxygen` 0 warnings, `abi-check` OK |
| Commits | 79 on this branch, **every one green at its end** |

The frame ABI work is **done and tested on all three backends**: one typed spelling in
both directions, frames invisible above the call cache, no partial modules anywhere,
and every unsafe shape refused with a reason.

## 2. What to fix first: the three defects

Nothing in this list has been touched -- the sweep was read-only by instruction.

### D-1 -- LibJIT: an UNCAUGHT `throw` returns normally

* **Evidence:** `[exceptions]` probe (pinned) + probe 13 (handlers work) + probe 21
  (payloads cross instances).
* **Narrow:** caught throws are correct; only the uncaught case returns instead of leaving
  a pending trap.
* **First step:** find where LibJIT's dispatcher handles `throw` with no matching handler
  in the current frame; it must set the pending-trap status the other error paths use
  (`WasmTrap.hpp`), not fall through.

### D-2 -- LibJIT: `array.set` / `array.fill` ABORT the process

* **Evidence:** `Assertion '!valueStack.empty()' failed` at
  `LibjitOpcodeDispatcher.cpp:1783` (`popValueRaw`).
* **Worst of the three:** an abort cannot be caught, so the probe skips LibJIT and says
  so.
* **First step:** a minimal reproduction (`array.new_fixed` + `array.set`), then the
  operand count in the lowering -- the assertion says the stack is popped more times than
  it is pushed.

### D-3 -- SLJIT: `externref` through a STRUCT FIELD returns non-null for null

* **Evidence:** probe 16 (pinned). Probe 19 shows the same reference through an **array**
  round-trips correctly, so this is **one lowering path**.
* **Likely cause:** the backend returns the GC struct it just built instead of the
  payload read from its field.
* **First step:** compare `struct.get` codegen for a reference field against an `i32`
  field.

## 3. Feature gaps (honest refusals, all temporary)

| Gap | Backends | Note |
|---|---|---|
| exceptions (`throw`, `try_table`) | SLJIT, AsmJIT | refused by dispatch; LibJIT shows the target behaviour |
| atomics | SLJIT, AsmJIT | consistent refusal across `rmw.add`, `store`, `load` |
| frame-form tail calls | SLJIT, AsmJIT | SLJIT needs a frame-carrying trampoline (6th thunk arg) |
| bulk memory on memory64 | SLJIT, AsmJIT | message says "yet" -- a scheduled limitation |
| LibJIT frame-mode calls / imports / multi-result | LibJIT | **policy**, not limitation: frame mode is opt-in and partial on purpose |
| LibJIT frame-mode multi-result | LibJIT | deferred by decision; the refusal is the contract |

## 4. Process hazards this campaign paid for (read before editing)

1. **Grep for the guard before writing it.** Three times a "missing" guard already
   existed (`kMaxWasmParameters`, SLJIT's multi-result check, the exit to `unsupportedReason`).
2. **A sentinel means what the tree says.** A null `fnPtr` means both "unsupported" and
   "no compiled entries"; inferring support from it broke three tests.
3. **Instrument before theorising.** When two reverts agree, print; one probe settled what
   two edit rounds could not.
4. **A filtered green run is not a green tree.** Two commits in this campaign went in red
   because a filtered run passed; run `--rng-seed 12345` over the whole suite.
5. **Validate the fixture before believing it.** Three probes measured themselves (invalid
   `ref.func`, missing `array.fill` count, a `get` on a consumed array).
6. **Do not project a known defect into untested territory** -- D-3 was nearly filed as
   "SLJIT mangles references" instead of "SLJIT mangles a reference in a struct field".
7. **Mechanical:** the editor tool doubles `\` in Doxygen commands, and inserting a `/*! */`
   block above a documented declaration makes Doxygen merge both docs onto one function.
   After any header edit: run `doxygen` and expect to reorder.

## 5. Test-organisation rules in force

* **No partial modules** -- any backend that cannot compile a function throws at module
  level, naming the function and the reason.
* **Pin, don't skip** -- a known defect is asserted *as the wrong behaviour*, so closing it
  forces the test to be rewritten (D-1, D-3).
* **Refusal is a pass** in a probe -- `continue` after printing, never `SUCCEED`.
* **Assert the property**, not the outcome (round-trip identity, not "it returned").

## 6. Next session's recommendation

**Fix D-1 first.** It is the narrowest (one path), it is well understood (handlers work,
payloads work, only the uncaught exit is wrong), and it removes the only cell in the
capability matrix where an embedder gets a wrong answer silently. D-2 second (it is the
worst behaviour, an abort), D-3 third (it is pinned and narrow).

After the defects: the capability matrix's remaining ⛔ rows, in the order the project
cares about -- exceptions on SLJIT/AsmJIT, then atomics, then the frame-carrying
Trampoline for tail calls.
