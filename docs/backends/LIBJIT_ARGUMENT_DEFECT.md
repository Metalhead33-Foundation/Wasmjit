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

