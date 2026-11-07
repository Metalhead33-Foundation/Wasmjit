# ABI refactor: post-mortem {#doc_abi_refactor_post_mortem}

**Scope:** the `v128` boundary work merged as PR **#11** (`AbiRefactor` → `backends`),
plus the two parallel follow-ups now starting (AsmJIT result buffer, SLJIT
stack-passed argument slots).
**Branch of record:** `backendsMh33`.
**Companion documents:** `ABI_REFACTOR_STUDY.md` (the chronological log, §1-§57),
`backends/V128_SIGNATURE_DECISION.md` (the decision and its compliance registry),
`backends/LIBJIT_ARGUMENT_DEFECT.md` (the one open defect).

## 1. What shipped

| Layer | Outcome |
|---|---|
| Decision | `v128` crosses as **two scalar words, low first**; a `v128` result goes through a caller-supplied buffer. `argSlotCount`/`mapArgSlots` in the core state it. |
| **SLJIT** | parameters **canonical** (two `W` slots, pinned); results **canonical** (16-byte out-buffer, working and tested) |
| **AsmJIT** | parameters **canonical** (two `kUInt64`, asserted) |
| **LibJIT** | **recorded deviation** `D-L1`: one 16-byte aggregate, because libjit loses the sixth integer argument |
| Host boundary | `WasmCallCache` asks the module instance how the backend spells a `v128` and describes the matching ffi shape |
| Policy | cross-backend calls are **illegal**, which is what lets the three spellings coexist |

## 2. What went well

1. **The decision note first.** Writing the rule (and its portability argument)
   before any code meant every later disagreement had a reference to test
   against, and every deviation had to be recorded rather than absorbed.
2. **Pins before changes.** Both boundary pins (`[libjit][v128][order]`,
   `[sljit][v128][order][abi]`) were written *before* the change they guard, so
   "it still passes" meant something. The SLJIT pin deliberately calls through
   the **canonical** declaration, which is why it proved compliance rather than
   coincidence.
3. **Ordering by "what a change can displace".** SLJIT went first not because it
   was easy but because it had the least working behaviour to break; LibJIT went
   last because it had the most. That rule found the right order after three
   wrong ones.
4. **IR-level unit tests with no backend code.** Eleven `[libjit][v128][ir]`
   cases eliminated the representation, the prologue pattern, the optimiser, the
   helper-call boundary, the emission order, the driver and libjit's own stack
   handling -- each in one build, each permanent.
5. **Dump instead of guess.** `jit_dump_function` ended a chain of hypotheses in
   one read, and turned "libjit is probably broken" into "our IR is correct and
   the loss is in codegen".
6. **Refusal beats corruption** and beats aborting: SLJIT now declines a
   signature it cannot express instead of tripping its own assertion and taking
   the process down.

## 3. What went badly

| Failure | Cost | Root cause |
|---|---|---|
| Four LibJIT attempts crashed before any pin worked | days | The plan was written before the code that implements it was read |
| Three plan reorderings (2+3 → 2+3+4 → backend order inverted) | one round each | Subtask boundaries drawn through edits that are simultaneous by construction; no measurable gate exists between them |
| Two rounds of "producer before consumer" | two rounds | A predicate/trigger was widened before the path behind it existed |
| The **wrong overload** trap, hit twice | two rounds | `LibJitTypeTranslation.cpp` has two `translateFunctionSignature` implementations and only one is live |
| A shared fixture carried a shape SLJIT cannot compile, twice | one round | "Backend-specific shapes need backend-specific fixtures" was learned, written down, then violated again |
| §50 blamed libjit's *stack* arguments; the real fault was `r9` | one round | A plausible mechanism was accepted without a discriminating measurement |
| A `qmake Test/Test.pro` overwrote the top-level Makefile | one round | Tooling assumption (which `.pro` owns the build dir) never verified |

The common thread is not carelessness about code -- it is **order of
operations**: reading after planning, measuring after concluding, and changing
producers before consumers. Every one of those is cheap to prevent and expensive
to discover.

## 4. Metaphor worth keeping

The suite was green while the code was wrong at least three times: LibJIT's
aggregate assumption, SLJIT's refusal test enshrining a non-canonical
convention, and the spec harness skipping the degenerate-shift commands. A green
suite is evidence of *what it tests*, never of what it does not.

## 5. Numbers at merge

| Fact | Value |
|---|---|
| Suite | 21717 assertions, 746 cases |
| Header/IR pins added for the boundary | 11 `[libjit][v128][ir]`, 2 boundary pins, 1 cache-flag pin, 1 refusal pin |
| Backend deviations recorded | 3 (`D-L1` LibJIT parameters; AsmJIT `v128` result; MS x64 provisional) |
| Studies/notes produced | 57 study sections, 1 defect note, 1 post-mortem |

## 6. Follow-ups, in the order this post-mortem recommends

1. **AsmJIT `v128` result → caller buffer.** Last canonical deviation. Mirror
   SLJIT's implementation: trailing pointer argument, `void` return, body copies
   16 bytes. Test it the way SLJIT's was tested -- a pre-filled buffer that must
   come back zeroed, so a no-op fails.
   *Measured 2026-11-06:* the backend did not miscompile the shape, it
   **refused** it at instantiation ("AsmJit supports only i32/i64/f32/f64
   results") -- a capability addition, not a bug fix.
   **Done 2026-11-06:** the buffered path is implemented (signature, prologue slot,
   return copy, call cache) and pinned by `[asmjit][v128][abi]` with a pre-filled
   buffer. Two paths are still deliberately refused and are the remaining work: a
   *wasm-level call that receives* a `v128` result, and the host `callCallable`
   path, which needs the same buffer argument (a limitation SLJIT shares).
2. **SLJIT stack-passed argument slots.** SLJIT currently *refuses* signatures
   needing more than six argument words (three `v128` parameters). Implementing
   §36's plan restores that capability: cap the declared saved registers, and
   read the overflow slots from the incoming stack. The offset rule is per-target
   and must be verified, not assumed -- a wrong offset is a silent miscompile.

   **Status 2026-11-06: not started, deliberately.** The two halves are not
   separable: capping `savedCount` without a correct stack read turns a clean
   refusal into a wrong answer, which is strictly worse. So the unit starts with
   a probe, not an edit.

   **Measured 2026-11-06: the plan is not implementable.** The probe was run --
   with SLJIT directly, seven declared integer arguments, six saved registers --
   and `sljit_emit_enter` **aborts** in `function_check_arguments`. SLJIT cannot
   *declare* an argument slot that has no saved register, so there is no stack
   slot to read from: the API has no way to name one. The backend's refusal at
   that same threshold is therefore the ceiling, not a temporary gap, and lifting
   it needs a change in SLJIT itself. Pinned by `[sljit][args][abi]`, which
   asserts the refusal threshold equals `SLJIT_NUMBER_OF_SAVED_REGISTERS`.

   **Correction, same day.** That measurement proves one thing precisely --
   SLJIT cannot **declare** an argument that has no saved register -- and it does
   **not** prove the seventh argument is unreadable. A second probe declared only
   six slots (legal) while the C caller still passed seven, sweeping offsets from
   `SLJIT_SP` for the value; it aborted in the same check for a *different*
   reason: the register budget it requested (4 scratches + 6 saved) is itself
   over the limit on this target. So the read path is **untested, not refuted**,
   and the honest status is: the *declaration* route is closed, the
   *read-the-incoming-stack* route is open and needs the budget tuned
   (fewer scratches, or `sljit_get_local_base` if `SLJIT_MEM1(SLJIT_SP)` is not
   the right base for the incoming-argument region).

   *Third-party suggestion, considered.* "SLJIT allows at most three
   parameters" comes from the old tutorial's `SLJIT_CALL0..3` macros. This build
   exposes `SLJIT_ARGS0..4`, our backend already emits helper calls with three
   and four arguments, and it compiles functions with up to six declared
   argument slots -- so the claim does not describe the API we use. The
   accompanying local-stack advice (`sljit_emit_enter(..., local_size)` plus
   `SLJIT_MEM1(SLJIT_SP)`) is what the second probe above is built on.

   *Original probe design, kept for the record.* Build a
   seven-integer-argument function with SLJIT directly, return
   `SLJIT_MEM1(SLJIT_SP)` at a candidate offset, and sweep the offset until the
   call `fn(nullptr, 1, 2, 3, 4, 5, 42)` returns `42`. That gives:

   * the offset of the **first stack-passed word** relative to `SLJIT_SP` after
     `sljit_emit_enter(..., local_size)`,
   * whether it moves with `local_size` (so the formula can be written in terms
     of the frame the backend already computes),
   * and whether a *scalar* stack word and the high half of a `v128` behave
     identically -- the `r9` investigation taught that "plausible" is not
     "measured".

   Only then does `beginFunction` change: cap the declared saved registers at
   `SLJIT_NUMBER_OF_SAVED_REGISTERS`, and for each parameter whose slots lie
   beyond the budget, load from the measured offset instead of `SLJIT_S(n)`. The
   exit gate is the existing `[sljit][v128][abi][shapes]` test flipping from
   "refused" to "`three_lanes` returns 6".

3. **libjit defect.** Report upstream with the closing experiment from the defect
   note (disassemble the entry, watch `r9` before the first store). If fixed,
   `D-L1` can be retired and the rehearsed patch set re-applied.
4. **Adopt `mapArgSlots` in SLJIT.** Its docstring warns that hand-rolled slot
   walks caused three investigations; SLJIT still hand-rolls one.

### Next two probes, in order

**A. SLJIT: can the callee READ a stack-passed argument?** Deferred; declaration
is measured closed, reading is untested. Plan: sweep again with a legal register
budget (the first attempt aborted for asking 4 scratches + 6 saved, not because of
the arguments); declare only what fits, let the C caller pass seven, and sweep
`SLJIT_MEM1(SLJIT_SP)` offsets for the value 42; if that base does not reach the
incoming-argument region, sweep from `sljit_get_local_base` instead. Exit gate
unchanged: `[sljit][v128][abi][shapes]` flips from refused to `three_lanes` = 6.

*Probe attempted again 2026-11-06 and still inconclusive, with a sharper
question.* With six declared slots, `saveds = 6`, four scratches and
`SLJIT_ENTER_FLOAT(3)`, `sljit_emit_enter` still aborts in
`function_check_arguments` -- yet the arithmetic that check is assumed to do
`scratches + saveds = 10 <= SLJIT_NUMBER_OF_REGISTERS = 12` passes. So the contract
of `function_check_arguments` is **not** what this study has been modelling, and
the next move is to read that function's body in `sljitLir.c` rather than keep
varying the numbers. Note that the backend itself compiles five-slot signatures
with `saveds = 5`, which is why the two-vector pins pass while this probe does
not.

### What `function_check_arguments` actually says (read, not guessed)

`extern/sljit/sljit_src/sljitLir.c:924`. The body hard-caps the argument list:

```c
while (arg_types != 0) {
    if (word_arg_count + float_arg_count >= 4)
        return 0;                       /* at most four arguments, period */
    ...
}
return (saved_arg_count <= saveds && scratch_arg_end <= scratches
        && float_arg_count <= fscratches);
```

So SLJIT's documented-by-code limit is **four arguments in total**, not a saved-register
count. That matches the third-party note that prompted this reading (it said three; the
code says four) and explains why sweeping saved-register counts never helped: the ceiling
was never `saveds`.

**But it contradicts our own passing pins.** `two_vectors` declares context + four words
-- five arguments -- and compiles and runs correctly, and so does the `v128`-result pin
with a trailing buffer. Both should trip the `>= 4` cap. Two readings are possible and
they are separable in one build:

1. `SLJIT_ARGUMENT_CHECKS` is compiled out in our SLJIT build and the abort our probe hit
   came from a *different* check in the same macro block; or
2. our `argTypes` really is four entries and the fifth slot is something else.

Decisive probe: print `argTypes`, the scratch count and the saved count from
`beginFunction` for a **working** `two_vectors` compile, and print the same for a failing
probe configuration. One build, two lines of output, no guessing.

### Resolved: SLJIT's ceiling is four argument slots, and the backend was too permissive

The temporary probe settled the contradiction in one run. Printing `argTypes`,
`nextSlot - 1` and `savedCount` from `beginFunction` for the **working** pins gave:

```
ENTER argTypes=0x11232 slots=4 saveds=4     (scalar then vector)
ENTER argTypes=0x21132 slots=4 saveds=4     (vector then scalar)
```

Four slots -- exactly the `>= 4` cap in `function_check_arguments`. So nothing we compile
today ever exceeds it: every earlier "five slots work" belief came from fixtures that had
since been removed. The contradiction was in the study's memory, not in SLJIT.

**Consequence: the backend's refusal threshold was wrong.** It refused above
`SLJIT_NUMBER_OF_SAVED_REGISTERS` (6), so a five- or six-slot signature would have passed
the check and then **aborted the process** inside `sljit_emit_enter`. It now refuses above
`kArgumentSlotCeiling = 4`, with the reason in the constant's comment and a pointer at
`sljitLir.c:924`. That turns a latent crash into a clean, diagnosable refusal -- three
words after the context is the real capability, not five.

### Calling convention: all three backends already use the host C ABI

Asked and answered from the source, because it is the assumption the whole `v128` rule
rests on:

| Backend | What the generated function actually is |
|---|---|
| **LibJIT** | `jit_abi_cdecl` in every `jit_type_create_signature` |
| **AsmJIT** | `asmjit::FuncSignature(asmjit::CallConvId::kCDecl)` everywhere |
| **SLJIT** | `sljit_emit_enter(compiler_, 0, ...)` -- options `0` selects the platform's default C ABI |

So yes: every compiled wasm function is a **standard host-C-ABI function**, callable from
native C++ exactly as the harness and the tests do. That is precisely why the two-word
spelling and a 16-byte aggregate are the same ABI on SysV, and why `callCallable`,
libffi and `jit_apply` can all reach the same entry.

**Therefore the four-argument cap is not an ABI limitation, it is an SLJIT API limitation**
-- and it is the kind every real SLJIT user works around by passing **one pointer to a
context struct** instead of many arguments (the pattern PCRE2's JIT uses). That maps
directly onto the staging-area rule sketched in section 11.3 of the LibJIT defect note: a
signature that needs more words than the API accepts carries them through memory,
referenced by a single pointer. The ABI stays C; only the *shape* of the argument list
changes.

*The name is the warning.* SLJIT is the **Stack-less** Just-In-Time compiler: it
does not own a stack of the kind that carries arguments, so a callee reaching its
caller argument area is exactly what one must not assume. That is why this stays
a probe and not a patch.

**B. AsmJIT: a wasm-level call that RECEIVES a v128 result.** Attempted
2026-11-06 and **reverted**: relaxing the two call-result refusals and filling the
trailing buffer argument inside `emitCallResult` compiled, but instantiation
failed with `AsmJit compiler finalisation failed: 25`. Hypothesis: the buffer
address is set on the `InvokeNode` *after* the node was created, and AsmJIT does
not accept that for an argument beyond the ones `setCallArguments` filled. Next
attempt should pass the buffer through `setCallArguments` (i.e. append it to the
prepared argument list before `compiler.invoke`), not through a later `set_arg`.
The refusals are back in place, so the shape is still honestly refused.

## 7. Practice changes adopted

- Read the code a plan depends on **before** writing the plan; a plan that cites
  a line number is worth two that cite a function name.
- Every claim gets a measurement with a number in it, or it is labelled
  *provisional* in the same sentence.
- Order subtasks by **what they can displace**, not by apparent difficulty.
- A deviation is a documented, pinned, dated record -- never a silent absorption.
- Backend-specific test shapes go in backend-specific fixtures.

