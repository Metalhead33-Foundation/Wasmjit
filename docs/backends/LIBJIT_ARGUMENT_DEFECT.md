# LibJIT: the sixth integer argument is corrupted (defect note)

**Status (2026-11-06):** open, reproducible, isolated, and **not** ours to fix in IR.
**Scope:** the LibJIT backend only. It appears when a compiled function's C
signature needs **more than five integer argument words after the `VMContext*`**,
i.e. when a wasm function declares **three or more `v128` parameters** under the
canonical two-word rule, or any equivalent mix of scalars and vectors.

## 1. Symptom

Under the canonical boundary (`docs/backends/V128_SIGNATURE_DECISION.md` §28,
two 64-bit words per `v128`, low first), a LibJIT-compiled function with three
`v128` parameters returns a corrupted **third** parameter:

| Export | Body | Expected | Measured |
|---|---|---|---|
| `ret_p0` | `local.get 0` | `{11,11,11,11}` | correct |
| `ret_p1` | `local.get 1` | `{22,22,22,22}` | correct |
| `ret_p2` | `local.get 2` | `{33,33,33,33}` | `lane0 = 1867730888`, `lane3 = 33` |

The third vector's **high word is correct and its low word is garbage**. Counting
argument words (`ctx`, then two per vector) those are the **6th and 7th**
arguments: the corrupted one is the **6th**, the last argument SysV passes in a
register (`r9`). The 7th is stack-passed and arrives intact.

## 2. What the generated IR actually says

`jit_dump_function` on the failing function:

```
function wasm(l1 : ptr, l2 : ulong, l3 : ulong, l4 : ulong, l5 : ulong, l6 : ulong, l7 : ulong) : struct<16>
	incoming_reg(l1, rdi)   incoming_reg(l2, rsi)   incoming_reg(l3, rdx)
	incoming_reg(l4, rcx)   incoming_reg(l5, r8)    incoming_reg(l6, r9)
	incoming_frame_posn(l7, 16)
	l16 = &s15 ; store_relative_long(l16, l2, 0) ; store_relative_long(l18, l3, 8)
	l21 = &s20 ; store_relative_long(l21, l4, 0) ; store_relative_long(l22, l5, 8)
	l25 = &s24 ; store_relative_long(l25, l6, 0) ; store_relative_long(l26, l7, 8)
	outgoing_reg(l1, rdi) ; call_external wasm_stack_check
```

Every element is correct: the canonical signature (pointer + six `ulong`), the
register/stack split (`l6 -> r9`, `l7 -> stack[16]`), the pairing used by the
third aggregate's stores (`l6` at offset 0, `l7` at offset 8), and the ordering
(all six stores before the first `call_external`).

**Conclusion: the IR is right. The value is lost later, in libjit's code
generation / register allocation for this IR.**

## 3. What has been ruled out by measurement

Each of these is a permanent `[libjit][v128][ir]` case in
`Test/LibJitBackendTests.cpp`, so none can be re-litigated:

| Candidate | Test | Result |
|---|---|---|
| libjit mishandles stack-passed integer arguments | 7 arguments, return the 7th | **correct** (`42`) |
| libjit mishandles a stack argument stored through an address | 7th arg -> `address_of` temp | **correct** (`42`) |
| the prologue's assemble pattern | three aggregates from six words | **correct** (`6`) |
| register/stack pressure in the body | same + eight live temps | **correct** (`34`) |
| reads scheduled late | lanes read after the pressure block | **correct** (`34`) |
| the optimiser | levels 0, 1, 2, 3 | **correct** (`6`) at every level |
| the helper-call boundary | three native calls taking each address | **correct** (`6`) |
| the emission order | all stores first, then all calls | **correct** (`6`) |
| the driver | `rawFnPtr` vs `fnPtr` | **both wrong**, differently |
| opcode lowering | pass-through exports, no SIMD op | **wrong** -- so not an opcode |
| the signature translation | dump of `translateFunctionSignature` | **correct** (7 params, 6 `ulong`) |

Six hand-built replicas with the same signature, the same stores, the same
helper calls and the same ordering all return correct results. The corruption
appears **only** in the compiled wasm function.

## 4. Cause (as far as it is established)

The loss is consistent with **libjit spilling `r9` (the last incoming argument
register) incorrectly** in this function shape: the value is available at entry,
is consumed by the first store in IR order, yet the stored word is garbage --
while the stack-passed word, which libjit explicitly places at
`incoming_frame_posn(16)`, is stored correctly.

What distinguishes the real function from every working replica is the rest of
its body: three `call_external` helper calls (stack check, trap slot, exception
slot) and their pending-trap probes, immediately after the stores. Those force a
frame and a spill pattern the replicas do not reproduce exactly.

Prime suspects, in order:

1. libjit's own function prologue clobbering `r9` before it saves the incoming
   arguments to their frame homes;
2. a spill/reload of the `r9`-sourced value placed after a `call_external` that
   clobbers `r9` (caller-saved), i.e. a live-range error in the allocator.

Both are inside libjit, below our IR.

## 5. Impact

* Two `v128` parameters (five words) are **correct**.
* Three `v128` parameters (seven words) corrupt the third one's low word.
* Affected spec scripts: `simd_bitwise` (`bitselect`), `simd_f64x2_arith`
  (`add-sub`, `div-add`, ...), `relaxed_laneselect` (all six), the `i16x8` arith
  families -- the 22 failures left after the harness fix.

## 6. Workaround options (LibJIT-only, to be evaluated)

Each is local to the LibJIT backend or its compilation policy; none changes the
canonical boundary or another backend.

1. **Compile affected functions at optimisation level 0.** If the allocator's
   live-range analysis is the culprit, `-O0` may not run it. The replicas already
   pass at every level, so this only becomes informative on the compiled wasm
   body.
2. **Two-hop parameter copy.** In the prologue, first `jit_insn_store` each
   incoming word into a fresh `ulong` local, then store that local into the
   aggregate. This ends the argument's live range immediately after entry, which
   defeats both suspects above if either is a live-range error.
3. **Frame-shape pressure.** Materialise the aggregate slots (and any padding)
   *before* reading parameters, so libjit's frame layout is fixed before the
   incoming registers are live.
4. **Keep the sixth word out of a register.** For signatures needing six or more
   words, apply (2) only to the word that would land in `r9`.
5. **Reject in the backend.** Detect the shape at declaration time and refuse to
   compile it, with a diagnostic -- the honest fallback, and strictly better than
   silent corruption.

Option 5 is acceptable without further measurement; 1 and 2 can each be decided
in a single build.

## 7. Workaround experiments run (2026-11-06)

| Option | Change | Result |
|---|---|---|
| 1 | body compiled at optimisation level 0 for signatures with >6 parameters | **no effect** -- the third vector's low word is still garbage |
| 2 | two-hop copy (each incoming word stored into a fresh `ulong` local, then into the aggregate) | **no effect** |
| -- | `rawFnPtr` vs `fnPtr` for the same function | **both wrong, differently** -- the driver is not involved |

Options 3 and 4 are variants of 2 and are not expected to behave differently,
since 2 already ends the argument's live range at entry and the value is still
lost.

Together with the measurement in §3, this means the value is lost **before any
IR we can write executes** -- i.e. inside libjit's own function prologue, while
it saves the incoming argument registers. If that is so, **no backend-side IR
can recover it**, and no wrapper or trampoline can either: the word never
arrives.

## 8. Recommendation

Two workarounds remain viable, and the choice between them is a product
decision rather than a technical one:

1. **Refuse the shape (option 5).** Detect a signature needing more than five
   argument words after the context at declaration time and refuse to compile
   that function, with a diagnostic. Effect: no silent corruption; the affected
   spec commands skip rather than fail; capability is honest but reduced
   (three-vector functions stop existing). Cost: one guard in the backend plus
   a test.
2. **Restore the backend's own spelling for those shapes.** LibJIT historically
   passed a `v128` as a single 16-byte aggregate, which is four arguments for a
   three-vector function and therefore never reaches `r9`. Restoring that
   **only for LibJIT** removes the defect outright and costs nothing at
   runtime -- but it makes LibJIT's boundary differ from the canonical rule for
   these signatures, which is exactly what `docs/ABI_PORTABILITY.md` forbids
   for cross-backend interop (MS x64 passes such aggregates by reference).

Given that the canonical rule exists to keep backends interchangeable, option 1
is the defensible default and option 2 is the escape hatch to take only with an
explicit milestone decision recorded against
`docs/backends/V128_SIGNATURE_DECISION.md`.

### 8.1 Decision (2026-11-06): option 2, LibJIT-only

**Choice: LibJIT keeps its own 16-byte aggregate spelling for a `v128`
parameter.** The canonical two-word rule remains the cross-backend contract and
is implemented by SLJIT (parameters and results) and AsmJIT (parameters);
LibJIT is a recorded deviation.

Why this is safe on the tested target, and where it is not:

* SysV AMD64 classifies a 16-byte aggregate as **two INTEGER eightbytes**, i.e.
  exactly the two registers the canonical spelling would use. Native host
  imports, cross-backend calls and the spec harness therefore behave identically
  under either spelling on x86-64 -- this is why every pre-refactor vector test
  passed with the aggregate and why the harness needed no change for it.
* MS x64 passes such an aggregate **by reference**, so a LibJIT-compiled
  function would not be callable from an MS x64 caller expecting two words.
  LibJIT is therefore **provisional on MS x64** for signatures that carry a
  `v128`, which is recorded here rather than discovered later.

Pinned by `[libjit][v128][abi][shapes]`: three `v128` parameters arrive intact,
both as a pass-through and through a lane read.

### 8.2 Why not the stack (the SLJIT answer, considered)

The SLJIT workaround is "read the excess argument slots from the stack instead
of from a register" (`docs/ABI_REFACTOR_STUDY.md` §36). That is unavailable
here, and the reason is structural rather than a matter of effort:

* SLJIT emits its own prologue, so it chooses where each argument comes from.
* libjit's public API exposes no equivalent: a parameter is whatever
  `jit_value_get_param` returns, and the prologue is libjit's to emit. There is
  no way to ask libjit to take argument *n* from `incoming_frame_posn` instead
  of `incoming_reg`.

The aggregate spelling is the closest analogue that *is* available: choosing a
parameter shape with fewer register arguments keeps the sixth word out of `r9`
entirely -- the same goal (do not put everything in registers) reached through
the signature rather than through the prologue.

## 10. MS x64: what is broken, and what is merely different

### 10.1 The aggregate spelling is self-consistent everywhere

On MS x64 a 16-byte aggregate is passed **by reference** (the caller allocates
and passes a pointer). That is a different *rule*, not a different *shape*: a
LibJIT-compiled function and a LibJIT-originated call site both speak it, because
both take their signature from the same translator. So **inside LibJIT, the
deviation is coherent on every target** -- it is not a SysV-only construct.

*Provisional*: we have no MS x64 machine here, so "libjit implements by-reference
aggregate arguments correctly on MS x64" is untested. It is labelled provisional
in the same sense as the other untested targets in `docs/ABI_PORTABILITY.md`.

### 10.2 The only real breakage is cross-backend, and it is narrow

A `v128`-carrying signature means one thing to LibJIT (aggregate) and another to
SLJIT/AsmJIT (two words). On SysV those are the *same ABI* by coincidence, which
is why nothing has ever surfaced. On MS x64 they are not: aggregate means
"pointer", two words mean "two registers".

So the failure mode is exactly this: **a call whose caller and callee are
compiled by different backends, where the signature carries a `v128`** -- a
LibJIT module importing an SLJIT- or AsmJIT-compiled function (or the reverse).
Everything else is fine.

## 11. Options for MS x64

### 11.1 Cheap workaround (recommended for now): refuse the cross-backend case

Detect, when a callable from another backend is bound to a `v128`-carrying
signature, and **refuse to compile the call site** with a diagnostic that names
the signature and both backends. Cost: one check in the LibJIT call path (and
the mirror check in SLJIT/AsmJIT for calls into LibJIT), no ABI change, no effect
on any signature that has no `v128`.

Why this is the right *cheap* answer rather than doing nothing: it converts a
silent miscompile on Windows into an explicit, diagnosable refusal, and it is
the only option that is honest about a target we cannot test here. It also
leaves the fast path untouched: single-backend modules, which is every spec
suite and every ordinary embedder, keep working.

### 11.2 Canonical solution A: fix libjit, then re-enable the two-word rule

The canonical spelling for LibJIT is already written and rehearsed
(`docs/ABI_REFACTOR_STUDY.md` §29-§31): both `translateFunctionSignature`
overloads, the `address_of` prologue, the `paramSlots` driver walk, the six
call sites, and the harness expansion in `TestBackend.cpp`. It is blocked by one
libjit defect -- the sixth integer argument being lost (`r9`) -- and §9 names the
one experiment that would characterise it well enough to report upstream.

If libjit is fixed (or patched), LibJIT returns to the canonical rule with no
API change at all, and the MS x64 divergence disappears along with the
deviation. Cost: an external dependency, with no schedule we control.

### 11.3 Canonical solution B: amend the contract with a spill rule

The portable fix -- and the one that matches the intuition behind the SLJIT
workaround -- is to stop asking the register file to carry arbitrarily many
vector words. A rule of the form:

> A parameter list that needs more than *N* integer argument words passes the
> **whole** `v128` parameter set through a caller-allocated, 16-byte-aligned
> staging area, referenced by a trailing pointer parameter.

is implementable by every backend (memory, not registers), removes the
register-budget cliff entirely, and makes the LibJIT defect irrelevant because
the shape it breaks no longer occurs. Cost: it is a **contract amendment**
against `docs/backends/V128_SIGNATURE_DECISION.md` §28, needing a specified
staging layout plus work in all three backends -- which is precisely the
"20 turns of changing everything" the deviation exists to avoid. Worth doing
when a second backend needs more than five argument words (SLJIT's own `§36`
stack plan is the same problem from the other side).

### 11.4 Summary

| Target / situation | Status |
|---|---|
| LibJIT on SysV, any shape | **works** (aggregate == two words here) |
| LibJIT on MS x64, single-backend module | **provisional** -- coherent, untested |
| LibJIT on MS x64, cross-backend call with a `v128` | **broken** -- fix with §11.1 |
| Any target, after libjit is fixed (§11.2) | canonical, no deviation |
| Any target, after a §28 spill amendment (§11.3) | canonical, no register cliff |

## 12. Policy: cross-backend calls are illegal (2026-11-06)

**Decision: a call whose caller and callee are compiled by different backends is
not permitted at all.** Not "unsupported with care" -- illegal.

### What that removes

The §11.1 guard exists only to catch the cross-backend case, so it becomes
unnecessary: with the policy in force the hazard it would refuse cannot occur,
and the deviation stops being an interop problem. SLJIT and AsmJIT may then
speak the canonical two words while LibJIT speaks its aggregate, **without any
mismatch ever arising**, because no call crosses the boundary between them.

### What it does *not* remove -- and this is the part that decides MS x64

There is a second boundary that is **not** cross-backend: the **host boundary**.

`WasmBase/WasmCallCache.cpp` builds a libffi `cif` for a callable and, for a
`v128` parameter, emits **two `ffi_type_uint64`** -- the canonical rule, hard-coded
and independent of which backend compiled the callee:

```cpp
const std::uint32_t slots = argSlotCount(valueKindOf(parameter.value.opcode));
if (slots == 1) {
    entry.argTypes.push_back(ffiTypeFor(parameter));
} else {
    for (std::uint32_t slot = 0; slot < slots; ++slot)
        entry.argTypes.push_back(&ffi_type_uint64);   // two words, always
}
```

On SysV this is the same ABI as LibJIT's aggregate, so nothing shows. On **MS
x64** it is not: libffi passes two words in two registers while a LibJIT-compiled
callee reads a **pointer** to a caller-allocated copy. Same system, same backend
on both sides -- and still a mismatch.

### Answer to "does the policy validate LibJIT on MS x64?"

**No, not by itself.** It removes the cross-backend hazard and leaves exactly
one exposure:

| Path into a LibJIT-compiled function | MS x64 status |
|---|---|
| direct call, spelled as a 16-byte aggregate (`Callable::fnPtr`, our tests) | **coherent** -- both sides by-reference; provisional |
| direct call, spelled as two words | mismatch -- a caller error, avoidable by the documented rule |
| **through `WasmCallCache` (libffi), any `v128` parameter** | **mismatch** -- the cache always uses two words |

So LibJIT becomes valid on MS x64 only once the host boundary agrees with it,
which is one small, local change: make `WasmCallCache` ask the target backend
for its `v128` parameter spelling (aggregate vs two words) instead of hard-coding
`argSlotCount`, and emit `ffiTypeFor(aggregate)` for a backend that spells the
parameter as an aggregate. That is the same information the backend already
carries in its signature translator -- a capability query, not a new concept.

Until then: LibJIT on MS x64 is **provisional**, correct for direct calls, and
wrong for `v128` parameters passed through the call cache.

### 12.1 Implemented (2026-11-06): the cache asks the backend

The gap above is closed, provisionally, by making the host boundary follow the
same rule the rest of the seam already uses -- *ask the backend*:

* `WasmModuleInstance` carries `v128ParameterIsAggregate` (default `false`, the
  canonical two words) with a setter and a getter.
* The LibJIT backend sets it to `true` in `ModuleCompiler::compileFunctions`,
  immediately before it compiles anything, with the reason in the comment.
* `WasmCallCache` reads it: for a `v128` parameter it emits two
  `ffi_type_uint64` by default, and one 16-byte `FFI_TYPE_STRUCT` of two
  `uint64` eightbytes when the backend spells it as an aggregate. libffi then
  classifies that struct exactly as the target ABI requires -- INTEGER, INTEGER
  on SysV, by reference on MS x64.

The **result** side needed no change: `ffiTypeFor(V128)` already returned
`v128Type()`, a 16-byte, 16-aligned `FFI_TYPE_STRUCT` of four `uint32`, which is
the aggregate shape. So after this change both directions of a LibJIT host call
describe one aggregate, and the file has a single definition of that type.


Pinned by `[callcache][v128][abi]`: a LibJIT instance reports the aggregate
spelling, an SLJIT instance reports the canonical one.

**Provisional**: no MS x64 machine is available, so the *behaviour* of the MS x64
path is untested; only its *shape* is now correct by construction. What can be
said without a Windows host is that both sides of a LibJIT host call now agree on
one spelling instead of two, and that the flag is the single place that decides
it.





## 9. What would close this properly

Disassemble the compiled entry (`objdump` on the generated code, or libjit's own
machine-code dump) and look at what the prologue does with `r9` between entry and
the first store. If `r9` is clobbered there, the defect is upstream of every
workaround and should be reported to libjit; if it is saved correctly, the loss
is in our frame layout and option 3 becomes worth testing after all.


