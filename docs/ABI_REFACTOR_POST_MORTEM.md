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
3. **libjit defect.** Report upstream with the closing experiment from the defect
   note (disassemble the entry, watch `r9` before the first store). If fixed,
   `D-L1` can be retired and the rehearsed patch set re-applied.
4. **Adopt `mapArgSlots` in SLJIT.** Its docstring warns that hand-rolled slot
   walks caused three investigations; SLJIT still hand-rolls one.

## 7. Practice changes adopted

- Read the code a plan depends on **before** writing the plan; a plan that cites
  a line number is worth two that cite a function name.
- Every claim gets a measurement with a number in it, or it is labelled
  *provisional* in the same sentence.
- Order subtasks by **what they can displace**, not by apparent difficulty.
- A deviation is a documented, pinned, dated record -- never a silent absorption.
- Backend-specific test shapes go in backend-specific fixtures.

