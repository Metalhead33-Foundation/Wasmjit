# Staging call ABI: two arguments, one frame {#doc_staging_call_abi}

**Status:** proposal, 2026-11-06. Amends
[`V128_SIGNATURE_DECISION.md`](V128_SIGNATURE_DECISION.md) §28 (not yet adopted).
**Author of the idea:** the observation that a compiled function only ever *needs*
two arguments — the `VMContext*` and a pointer to an argument/result frame.

## 1. Why

Four independent defects and limits share one cause: we ask the **register file**
to carry a wasm call's whole argument and result shape.

| Problem | Where it bites | Root cause |
|---|---|---|
| `D-L1`: libjit loses the sixth integer argument (`r9`) | LibJIT | argument words in registers |
| SLJIT caps a function at **four** arguments | SLJIT | `function_check_arguments`, `sljitLir.c:924` |
| SLJIT cannot *declare* an argument with no saved register | SLJIT | same |
| `v128` results in a vector register (AsmJIT) | AsmJIT | result in a register class |
| `v128` parameter spelling differs per backend | LibJIT vs the rest | register/aggregate classification |
| Cross-backend calls are **illegal** | the seam | spellings cannot agree across backends |

Every row disappears if the arguments and results are **memory**, not parameters.

## 2. The contract

> **A compiled wasm function has the C signature `void fn(VMContext*, void* frame)`.**
>
> `frame` is a caller-allocated, 16-byte-aligned region. The first
> `argsBytes(ft)` bytes hold the parameter values in declaration order, each
> `v128` as 16 bytes little-endian, every other value as its natural width
> rounded up to 8. The next `resultsBytes(ft)` bytes receive the results in the
> same layout.

Consequences, stated plainly:

* **Two C arguments, always.** SLJIT's four-argument cap is never approached;
  nothing is ever stack-passed by the host ABI.
* **No register-class questions.** An aggregate, two words and a vector register
  all become the same thing: 16 bytes at an offset.
* **`D-L1` retires.** LibJIT's `r9` defect cannot occur because no parameter ever
  occupies `r9` beyond the context pointer.
* **The per-backend spelling divergence disappears**, so the cross-backend
  prohibition can be lifted.
* **`v128` results need no special case** -- they are bytes in the frame, exactly
  like a multi-value tuple already is.

## 3. Layout

The existing rules already define the bytes; nothing new is needed:

| Concern | Existing owner |
|---|---|
| tuple field offsets, alignment, `v128` = 16 bytes LE | `docs/ABI.md`, "The result tuple layout"; `WASM::aggregateLayout` |
| how many words a kind occupies | `argSlotCount` / `mapArgSlots` in `WasmBackendAbi.hpp` |
| the `v128` stored form | `WasmGcHeader.hpp` (16 bytes, 16-aligned, little-endian) |

New core helpers only, purely arithmetic:
`argsBytes(const FuncType&)`, `resultsBytes(const FuncType&)`, `frameBytes(ft)`,
and `argumentOffset(ft, parameter)` / `resultOffset(ft, index)`.

## 4. What each side does

**Caller** (host, or compiled code making a call): reserve
`frameBytes(ft)` bytes; write the arguments at their offsets; call
`fn(ctx, frame)`; read the results at `argsBytes(ft) + resultOffset(i)`.

**Callee** (a compiled wasm function): in the prologue, load each parameter from
`frame + argumentOffset(i)` into its local slot; in the epilogue, store each
result at `frame + argsBytes + resultOffset(i)` and return.

Both sides derive the offsets from the **same `FuncType`**, which is the property
that makes the ABI canonical: it is defined by bytes, not by registers, so it is
identical on SysV, MS x64, AAPCS64, RISC-V and MIPS.

## 5. What this costs

* **One memory round trip per call and per return.** For scalar-heavy code this
  is a real regression: today a three-argument call costs three registers, and
  would cost three stores plus three loads. That is the price of portability, and
  it should be measured (`docs/ABI_PORTABILITY.md` already ranks portability
  above performance, but the number belongs in the record).
* **Every backend's prologue and epilogue change**, and the core's invocation
  path (`callCallable`, the call cache, the harness) must speak the frame form.
* **Tail calls** need the frame pointer carried through the trampoline, or a copy
  from caller frame to callee frame.
* **This is a decision change**, not an implementation detail: the frozen
  surfaces in `docs/API_ABI_STABILITY.md` include the embedder-facing invocation
  shape.

## 6. Migration (additive, per backend)

1. **Core**: add the arithmetic helpers and a `FrameCall` capability on the
   backend seam. Nothing existing changes.
2. **Per backend**: emit the two-argument form behind that capability; keep the
   current form working. Each backend flips when *its* pins pass.
3. **Invocation**: `callCallableFrame(callable, ft, args...)` alongside the
   existing `callCallable`; the cache grows a frame mode.
4. **Retire**: once all three backends and the harness speak the frame form, the
   register-form spellings, `D-L1`, the SLJIT four-argument refusal and the
   cross-backend prohibition all go away together.

## 7. Why this beats the alternatives

| Alternative | Why not |
|---|---|
| keep per-backend spellings | four live divergences, and cross-backend calls must stay illegal |
| SLJIT context-pointer only | fixes SLJIT and leaves LibJIT's `r9` and AsmJIT's vector result |
| raise SLJIT's cap upstream | cannot be done from here, and does not touch the other three |

## 8. Gates

* `[staging][abi]` per backend: a function with `(v128, v128, v128) -> v128`
  round-trips through a frame on **all three** backends, including LibJIT --
  which is the case that cannot work today at all.
* Every existing pin keeps passing until its backend flips, then is re-pointed at
  the frame form rather than deleted.
* A cross-backend call test, un-skipped, once the policy can be lifted.

## 9. Migration log (after-action report)

**Step 1 -- core arithmetic. Done 2026-11-06.**
`frameValueBytes`, `frameArgumentsBytes`, `frameResultsBytes`,
`frameArgumentOffset`, `frameResultOffset`, `frameBytes` in
`WasmBackendAbi.hpp`, all derived from `argSlotCount` so the frame layout and the
register layout cannot disagree. Pinned by `[abi][frame]` with 14 assertions,
including the case that motivates the whole note: three vector parameters are
**48 argument bytes and two C arguments**.

*What went wrong:* the header-doc gate rejects a plain `//` banner in a `.hpp` --
every header comment must be Doxygen (`//!`). Caught on the first build, fixed in
place. Worth knowing before a larger diff.

**Step 2 -- the capability and the core primitive. Done 2026-11-06.**
`ModuleInstance::frameCallAbi()` (default `false`, with a setter) records whether
a module's functions use the frame form -- the same shape as the existing
`v128ParameterIsAggregate` / `v128ResultIsBuffered` facts, deliberately *not* a
new virtual on the backend seam, which is a frozen surface. `callCallableFrame()`
in `WasmValue.hpp` invokes `void fn(VMContext*, void* frame)` and reuses
`callCallable`, so the trap check is the existing one.

Pinned by `[abi][frame]`'s tripwire: no backend reports `frameCallAbi()` today, so
every register-form pin keeps meaning what it meant. The day a backend flips,
that expectation changes **deliberately** rather than by accident.

*What went wrong:* the helper was first inserted above `callCallable`'s definition
and did not compile; it now sits after it. And the tripwire initially looped over
AsmJIT, which refuses `v128` parameters outright -- an unrelated recorded gap --
so it covers the two backends that compile a vector signature today.

**Step 3 -- SLJIT emits the frame form. Design settled, not implemented.**

*Mode decision (settles the mixed-module question).* A module compiles **either
entirely** in the register form **or entirely** in the frame form, chosen at
`ModuleCompiler` time: if **any** internal function's signature needs more
argument slots than the register form allows (more than `kArgumentSlotCeiling` =
4), the whole module switches and `instance.setFrameCallAbi(true)` is recorded.
That keeps `frameCallAbi()` a module-wide fact -- which is what the core's flag
already is -- and means a module of narrow signatures is byte-for-byte unchanged,
so every existing pin keeps its meaning.

*Sites, in order.*

| # | Site | Change |
|---|---|---|
| 1 | `SljitOpcodeDispatcher.hpp` ctor | take `bool frameCallAbi`; store `bool frameAbi_`; add `std::vector<WASM::ValueKind> frameKinds_` and `sljit_sw frameResultOffset_` |
| 2 | `kReservedSlots` | 8 -> 9, plus `frameSlotOffset()` = `(localSlots_ + 8) * kSlotBytes`, holding the frame pointer |
| 3 | `beginFunction` -- argument list | frame mode: `SLJIT_ARG_RETURN(RET_VOID) | SLJIT_ARG_VALUE(P, 1) | SLJIT_ARG_VALUE(P, 2)` and `savedCount = 2`; skip the ceiling refusal |
| 4 | `beginFunction` -- prologue | store `SLJIT_S(1)` (the frame pointer) into the reserved slot; for each parameter load from `frame + frameArgumentOffset(frameKinds_, i)` into `localSlotOffset(i)` (16-byte kinds as two 8-byte moves), reloading the frame pointer into `R2` per parameter because helpers clobber R0-R3 |
| 5 | `frameResultOffset_` | `frameArgumentsBytes(frameKinds_)`, computed once per function |
| 6 | `emitReturn` | frame mode: store the single produced operand slot at `frameResultOffset_` (two words for a `v128`), then `return_void` |
| 7 | `finish` trap epilogue | frame mode returns void for every result kind |
| 8 | `SljitModuleCompiler.cpp:102` | compute the module-wide mode, pass it to each dispatcher, `instance.setFrameCallAbi(mode)` |

*Gate.* `[staging][sljit][abi]`: the three-vector fixture (`v128_multi.wasm`)
round-trips through a 56-byte frame -- three 16-byte arguments at 0/16/32, one
`i32` result at 48 -- and returns 6. Passing it makes `kArgumentSlotCeiling` and
the "four argument slots" refusal dead code in frame mode, which is the point of
the whole exercise.

*Not started:* the budget for this session ended at step 2. Nothing about the
above is uncertain; it is a mechanical edit with the sites named.




**Step 3 -- SLJIT emits the frame form. Done 2026-11-06.**
All eight sites landed as planned: constructor flag and `frameKinds_`, the ninth
reserved slot holding the frame pointer, the two-argument list with `nextSlot = 3`
and no ceiling, the prologue loading each parameter from
`frame + frameArgumentOffset(...)` (pointer reloaded into `R2` per parameter), the
result-block offset, the epilogue storing the result and returning void, the trap
epilogue returning void, and the module-wide decision in `SljitModuleCompiler.cpp`.

**The gate passes.** `[staging][sljit][v128][abi]` compiles the three-vector fixture
through a 56-byte frame -- arguments at 0/16/32, the `i32` result at 48 -- and reads
back **6**. SLJIT's four-argument ceiling no longer applies to a signature of any
width, because a signature no longer occupies arguments.

*What this changes about earlier work:* `kArgumentSlotCeiling` and the "cannot pass
N argument slots" refusal are unreachable in frame mode. They stay, because they
still guard the **register** form that narrow modules use -- which is also why every
pre-existing pin kept passing unchanged: the mode is chosen per module, and only a
module carrying a wide signature switches.

*What went wrong:* two attempts died on shell/Python quoting before the third used
the editor tool block by block, which applied cleanly first time. The lesson is
recorded in the post-mortem: multi-site patches are written with the editor, never
through a heredoc. The header's Doxygen gate then caught the new constructor
parameter lacking a `\param`, fixed in place.

### Step 3, gap found immediately after: call sites are not frame-form yet

Step 3 converts **callee signatures and prologues**. It does not convert the calls
*inside* a compiled body: those still hand register arguments to what is now a
frame-reading callee. The gate passed only because the three-vector fixture makes
no calls, so the gap was invisible to it.

That is a silent-miscompile class, so it is guarded now: in a frame-form module,
`call`, `call_indirect`, `return_call` and `return_call_indirect` all report
`onUnhandledOpcode`, which refuses the function with a diagnostic instead of
emitting a mismatched call. Pinned by the existing suite (no behaviour change for
register modules).

**Consequence for the plan:** steps 4 and 5 are *larger* than step 3, not smaller.
Each needs the six call sites (`LibjitOpcodeDispatcher.cpp` has 16 `callArgs`
references) to build a frame, write the arguments into it, call, and read the
results back -- plus the signature, prologue, epilogue and driver, exactly as
SLJIT needed. The right shape is therefore one shared core helper that builds and
reads a frame from the operand stack, used by every backend's call sites, and only
then the two flips. The step-6 list ("harness + cache") should also be read to
include **compiled-to-compiled calls**, which is where the real work is.

### Step 3b -- compiled-to-compiled calls, implementation brief

Read from `Sljit/SljitOpcodeDispatcher.cpp`:

* `dispatchCall` / `dispatchCallIndirect` -> `emitCall` / `emitCallIndirect` ->
  **`emitResolvedCall(calledType, callableReg)`**, which is the single place where
  arguments are loaded and the indirect call is emitted. **One conversion point,
  not six** -- provided `emitCallIndirect` also routes through it.
* `emitCall` currently refuses a callee with more than three parameters
  (**`kMaxWasmParameters`**, line 2037) and any `v128` parameter (line 2042) --
  the same ceiling, restated for calls.
* Arguments live in the operand stack as 16-byte slots (`stackSlotOffset(depth)`),
  so packing them into a frame means copying each value from its slot to
  `frame + frameArgumentOffset(argKinds, i)` (two words for a `v128`).

**Where the frame lives.** In the caller's own stack frame, after the operand
stack: a new member `maxCallFrameBytes_`, sized in the measure pass as the maximum
of `frameBytes(calleeArgs, calleeResults)` over every call site, added to
`frameSlots` at `sljit_emit_enter` time (rounded up to `kSlotBytes`). Its address
comes from `sljit_get_local_base(compiler_, reg, callFrameOffset(), 0)` -- it must
not be assumed to be `SLJIT_SP`, which points below the local area.

**Sites, in order.**

| # | Site | Change |
|---|---|---|
| 1 | measure pass | track `maxCallFrameBytes_`; add it to `frameSlots` |
| 2 | `callFrameOffset()` | new helper, frame offset of the scratch area |
| 3 | `emitCall` | in frame mode skip the three-parameter and `v128` refusals |
| 4 | `emitResolvedCall` | frame mode: pack arguments from the operand stack into the scratch area, obtain its address with `sljit_get_local_base`, call `(ctx, frame)`, then copy results from `frame + argumentsBytes` into the operand slots |
| 5 | `emitCallIndirect` | ensure it shares site 4 |
| 6 | tail calls | stay refused in frame mode until the trampoline carries the frame pointer |

**Gate.** A module with a wide signature *and* an internal call: one export calls
`(v128, v128, v128) -> v128` and returns its lane sum. That fixture does not exist
yet and is the first artefact this step should add.

**Why this is the right unit.** It is one conversion point plus one scratch sizing
rule. Once it lands, the LibJIT and AsmJIT flips become the same edit with a
different emitter, because the frame construction is identical in kind.

### Step 3b, first attempt: one real finding and one open crash

Implemented and reverted, 2026-11-06. What was built: the staging scratch area
(`callFrameOffset()` after the operand stack, 16 extra slots reserved in frame
mode), both call-path refusals relaxed, and the frame branch inside
`emitResolvedCall` -- pack the arguments from the operand slots, take the scratch
address with `sljit_get_local_base`, `icall (ctx, frame)`, then copy the result
back. The fixture `staging_call.wat` (an export calling a three-vector callee)
was added and built.

**The finding:** the `frameAbi_` guards added to `dispatchCall` /
`dispatchCallIndirect` on the day step 3 landed had to be **removed** for this
step -- and doing so changed the failure from "clean refusal" to an SLJIT
assertion, which is exactly the sequence the guard was designed to produce.

**The open crash:** `sljitLir.c:3325`, `FUNCTION_CHECK_DST` inside
`check_sljit_get_local_base`. The destination register passed to
`sljit_get_local_base` is rejected, even though frame mode declares four
scratches (R0-R3) and the call uses `SLJIT_R1`. So either the scratch count seen
by the checker is not the four we pass (`sljit_emit_enter(..., 4 |
SLJIT_ENTER_FLOAT(3), ...)`), or `sljit_get_local_base` takes something other
than a plain scratch register here. **That is a read, not a guess:** the next move
is to read `FUNCTION_CHECK_DST` and `sljit_get_local_base`'s contract before
retrying.

*Nothing else in step 3b is in doubt*: the scratch sizing, the packing loop, the
`icall` shape and the result unpacking were all written and compiled; the only
unknown is the address-taking primitive.

### Step 3b, second attempt: done. The crash was our argument order

The open crash was `FUNCTION_CHECK_DST` in `check_sljit_get_local_base`, and the
cause was not SLJIT's contract but **our call to it**:

```cpp
sljit_get_local_base(compiler, dst, dstw, offset)
```

`dstw` must be **zero** -- the function checks `(p != SLJIT_SP) && (i == 0)` -- and
the first attempt passed the *offset* in the `dstw` position. One transposed
argument, one assertion four layers down.

**Now working.** `[staging][sljit][abi][call]` instantiates `staging_call.wasm` -- an
export that takes three vectors and calls a callee that also takes three -- builds
a 56-byte frame, calls the export through `callCallableFrame`, and reads **6** from
the result block. That exercises the whole path in both directions: arguments
packed from the operand stack into the scratch area, `(ctx, frame)` called through
the resolved `Callable`, and the result copied back into the operand slots.

Details that mattered and are now in the code:

* the scratch area is 16 slots after the operand stack (`callFrameOffset()`), so a
  callee frame never disturbs a live operand;
* the scratch address is taken with **`sljit_get_local_base(..., 0, offset)`** and
  re-taken after the call, because `R1` is caller-saved;
* the two call-path refusals (`kMaxWasmParameters`) apply only to the register
  form, so a wide module may call a wide callee;
* `call` and `call_indirect` lost their frame guards; **tail calls keep theirs**,
  since the trampoline does not carry a frame pointer yet -- that is the remaining
  refusal in the frame form, and it is honest.

## 10. Policy update: cross-backend calls are legal but **forbidden in production**

With the staging frame, the ABI is **identical on every backend** -- two C
arguments and a byte-defined frame -- so the reason for the previous outright ban
(per-backend spellings that could not agree) no longer exists. The ban is lifted
**technically**: a call whose caller and callee are compiled by different backends
is no longer ill-formed.

It remains **forbidden in serious production code**, and the rule is empirical, not
stylistic: each backend's frame path is pinned only by its **own** tests, so a
cross-backend call combines two independently-verified halves into one
**unverified whole**. A divergence in a corner -- a `v128` result, a multi-result
block, an import trampoline -- would surface as memory corruption in a program that
"passed" each backend's suite separately. Until there is a dedicated cross-backend
suite (step 6's last gate), the deployment rule is **one backend per process, or per
module graph**. Permitted by the type system, refused by review: just because you
can does not mean you should.

The embedder-facing consequences of all of this -- the three call directions, the
frame layout, the GC-root caveat and the AsmJIT plan -- are collected in
[`EXPORT_SIGNATURE.md`](EXPORT_SIGNATURE.md).

## 11. Step 4 (LibJIT) and step 5 (AsmJIT): implementation briefs

Both are the same shape as step 3 plus step 3b. What differs is the emitter.

### Shared structural decisions

* **Mode threshold.** Frame mode when a signature needs more argument slots than
the register form can carry. For LibJIT that is more than **5** slots after the
context (the `r9` loss); for AsmJIT, more than `kArgumentSlotCeiling` = 4. The
mode is module-wide, set once, and recorded with `setFrameCallAbi(true)`.
* **Mode must be decided before type translation**, because the signature itself
changes shape. In LibJIT that means the decision belongs beside
`translateTypes`, not only in `compileFunctions`; in AsmJIT beside
`translateTypes` too.

### LibJIT sites

| # | Site | Change |
|---|---|---|
| 1 | `LibJitTypeTranslation.cpp`, **`CoreTypeView` overload** (the live one) | frame mode: `void(VMContext*, void*)`, two parameters |
| 2 | `LibjitModuleCompiler.cpp` `compileFunction` prologue | load each parameter from `frame + frameArgumentOffset`; a `v128` assembles through `address_of` as today, but from two frame words |
| 3 | same, result path | store results at `frame + frameArgumentsBytes`; `jit_insn_return(nullptr)` |
| 4 | `LibjitModuleCompiler.cpp` driver | forward `(ctx, frame)` -- the `paramSlots` walk disappears entirely in frame mode |
| 5 | `LibjitOpcodeDispatcher.cpp`, **16 `callArgs` sites** | in frame mode build a frame in a fresh jit temp, store the arguments, call `(ctx, addr)`, load the results back |
| 6 | tail calls / thunks | remain on the register form; guard them in frame mode as SLJIT does |

### AsmJIT sites

| # | Site | Change |
|---|---|---|
| 1 | `AsmjitTypeTranslator.cpp` `translateFunctionType` | frame mode: `(ctx, frame)` -> void |
| 2 | `AsmjitModuleCompiler.cpp` prologue | parameters from the frame; the `v128` buffer already exists, so it becomes a 16-byte copy |
| 3 | `emitCallResult` / return path | write results into the frame; `ret()` |
| 4 | `setCallArguments` / `prepareCallArguments` | build the callee frame in a stack buffer and pass its address **as the second argument** -- this is the place the earlier `executeCall` attempt got wrong by setting an argument after the node existed |
| 5 | `requireScalar` call sites | the `v128` refusals become irrelevant in frame mode |

### Gates

Same fixture shape for both: `staging_call.wasm` (export calls a three-vector
callee) must return 6 through a frame, and `v128_multi.wasm` (no call) must return 6
too. Passing both on a backend retires, for that backend, `D-L1` (LibJIT), the
`v128`-result special case (AsmJIT) and every argument-count refusal.

**Not implemented.** Steps 4 and 5 are each one step-3-plus-3b unit; the briefs
above exist so they are executed, not explored.

### Step 4a scope, clarified by reading (2026-11-06)

LibJIT's frame flip splits the same way SLJIT's did: **4a** gives the frame form
to functions, **4b** to calls inside a body. Reading `LibjitModuleCompiler.cpp`
adds two facts the earlier table did not carry:

1. **The mode must be set on the translator before `translateTypes` runs.**
   `ModuleCompiler::translateTypes` has both the module and the instance, so the
   decision lives there -- one line before
   `typeTranslator.translateTypes(module.typeIds, ...)` -- and the translator
   needs a `frameCallAbi_` setter plus the branch in its `CoreTypeView` overload.
2. **The driver's forwarding collapses to two values.** It currently walks
   `paramSlots` per wasm parameter; in frame mode the JIT signature has exactly
   two parameters, so the driver emits `callArgs[0] = ctx`,
   `callArgs[1] = jit_value_get_param(driver, 1)` and nothing else. That is the
   fifth place this ABI simplifies code instead of complicating it, alongside the
   vanishing slot walk, the vanishing ceiling, the vanishing `v128` result special
   case and the vanishing `r9` defect.

**4a must also guard the call sites.** LibJIT's dispatcher has no frame guard
today (SLJIT's was removed in 3b because 3b implements calls). For 4a the guard is
required from the start: `call`, `call_indirect`, the tail-call paths and the
thunks must refuse in frame mode, exactly as SLJIT's did between steps 3 and 3b.

Threshold for LibJIT: frame mode when `1 + sum(argSlotCount(param))` exceeds
**5** -- the last slot the register form was measured to deliver.

**Not implemented.** Nine edits across three files plus a fixture, which is one
focused unit; the brief above is what makes it mechanical.

### Step 4a, first attempt: arguments cross, the result does not (reverted)

Seven edits were written and **compiled**: the translator flag and setter, the frame
signature branch, the module-wide mode decision in `translateTypes`, the
prologue-from-frame, the driver's frame signature, its two-value forwarding, and
the result store. The gate then measured this:

```
FRAME0=100000001 FRAME1=100000001   <- param 0, lanes 1,1,1,1   (intact)
FRAME2=200000002 FRAME3=200000002   <- param 1, lanes 2,2,2,2   (intact)
FRAME4=300000003 FRAME5=300000003   <- param 2, lanes 3,3,3,3   (intact)
FRAME6=f6c65bd0                     <- result slot: a TRUNCATED POINTER, not 6
```

That is a genuinely useful failure. **The whole argument half of the ABI is
proven**: the module switched modes, the signature took `(ctx, frame)`, the
prologue read all three vectors out of the frame at 0/16/32, the driver forwarded
the frame, and nothing overwrote a live argument. The defect is confined to the
**return path** -- either the frame signature's return type is not the scalar it
should be, or the driver's `store_relative` of `callRet` stores a pointer-typed
value at `frame + 48`.

The branch was reverted rather than left half-wired; the tree is green.

**Next action is therefore a two-line unit test, not an investigation**: build a
`LibJitTypeTranslator` with `setFrameCallAbi(true)`, translate a three-vector
signature, and assert `jit_type_num_params(sig) == 2` and that the return type is
the scalar (not `void_ptr`). If the signature is right, the bug is the single
`store_relative` in the driver; if it is wrong, it is `translateCanonStorage` on
a result. Either answer is one edit.

### Step 4a, second attempt: the frame path works; the switch is policy (off)

Three defects were found and fixed by measurement, in this order:

1. **The wrong overload.** `translateTypes` uses
   `translateFunctionSignature(const WASM::FuncType&)`, *not* the `CoreTypeView`
   one -- measured by a signature-shape test that reported a 16-byte second
   parameter where 8 was required. Both overloads now spell the frame form.
2. **A `v128` local is written through its address.** `jit_insn_store_relative`
   on the aggregate *value* segfaults; `jit_insn_address_of` first, then store.
   Found by calling `rawFnPtr` directly, which is the only way to separate the
   prologue's fault from the driver's.
3. **Automatic switching is wrong for LibJIT.** A `v128` is one 16-byte
   aggregate here, so three vectors cross the register form intact
   (`ctx` + 3 arguments). Switching every wide-vector module wholesale produced
   **ten suite failures and a crash**: modules that worked stopped working.

After (1) and (2), the frame path itself was **verified end to end**: a direct
call to `rawFnPtr` with a hand-built frame returned **6**, and so did the
`callCallableFrame` path through the driver. What is *not* enabled is the
automatic switch -- `frame` stays `false` by policy, with both the reason and the
measurement in the code, and two tests pin it: the frame signature is
`(ctx, frame) -> scalar`, and a wide vector module stays on the register form.

**What this changes about the plan.** LibJIT has no local argument ceiling to
lift, so its motive for the frame form is *cross-backend parity*, not capacity.
That makes "when does a module switch" a step-6 policy question -- per module,
per call site, or only for embedder-facing entries -- rather than a threshold.
The frame machinery is in place and measured; the policy is what remains.

### The frame form is opt-in: `WASM::setFrameCallAbiOptIn()`

There is now a deliberate request, not an inference
(`WasmBase/WasmBackendAbi.hpp`): `setFrameCallAbiOptIn(true)` makes every module
compiled while it is set take the frame form, and `frameCallAbiOptIn()` reports
the preference. It is process-wide, because the alternative -- a per-module
threshold -- was measured to be wrong: LibJIT's aggregate spelling already
delivers wide vector signatures, so automatic switching broke ten working modules.

**Rules the flag encodes.**

* A backend must not switch by *measuring* signatures. The form has to be honoured
  end to end -- prologue, calls, driver, embedder entry -- and a silent switch
  changes behaviour of modules that already work.
* Default off. The register form stays the fast path wherever it is valid, which
  for LibJIT is every shape it compiles today.
* Opt-in is the fallback: when an embedder needs **one ABI everywhere** (a mixed
  SLJIT + LibJIT process, or a single marshalling path in its own code), it asks
  once and every backend answers the same way.

**Verified on LibJIT with the flag on**: `v128_multi.wasm` instantiates in frame
mode, `callCallableFrame` writes the three vectors at 0/16/32, and the result
block reads **6** -- the same fixture that started this thread. With the flag off
the same module stays on the register form and still returns 6, which is why both
tests are kept: one pins the default, the other pins the option.

## 12. P0: the LibJIT guard, and why it belongs one level up (reverted)

**The hole is real.** `LibJit/` contains no `frameCallAbi` reference outside the
prologue, the mode choice and the driver, so a frame-mode module containing a `call`
compiled the register form against a callee expecting `(ctx, frame)` -- a silent
miscompile, not a refusal.

The guard was written at the five call entry points (`dispatchCall`,
`dispatchCallIndirect`, `dispatchReturnCall`, `dispatchReturnCallIndirect`,
`dispatchReturnCallRef`), each throwing when `instance.frameCallAbi()`. Result:
**two `[staging][libjit]` failures and a segfault in the full suite**, reverted on
sight.

### What that means

Throwing from inside the dispatcher happens **while LibJIT is building a context**.
That phase is not exception-safe: a throw leaves the context half-built, and the
*destructor* or the *next* module then faults. The failures also appeared in tests
that never enable the frame mode, which is the signature of corrupted state rather
than a wrong condition.

**Therefore the guard belongs at the module level**: refuse at instantiation,
before any JIT context is built -- scan the function bodies for call opcodes, or
refuse by type when the module has both a frame-mode signature and any call. A
refusal that happens before code generation cannot corrupt anything.

That is also the shape AsmJIT's native-import exclusion needs (P0b), so the two
subtasks share one mechanism: **decide refusal eligibility at the module level, then
compile.**

### Priority note (from the project owner)

Lifting the cross-backend *call* ban is **optional**. What is mandatory is
**one spelling for exposing C++ functions to wasm and one spelling for calling
compiled wasm from C++, on every backend**. That makes ABI uniformity -- not
cross-backend mixing -- the thing this campaign is measured by, and it demotes the
cross-backend test suite from a gate to a nice-to-have.

## 13. P0a done: the module-level refusal, and a wrong theory corrected

**Landed:**

```cpp
if (frame && (module.functionBodies.size() > 1 || !module.importFunctions.empty()))
    throw std::runtime_error("LibJIT frame ABI: a call inside a frame-mode function is not implemented");
```

in `ModuleCompiler::translateTypes`, before any JIT context exists. Coarse on
purpose: a module with one function and no imports cannot contain a call.

**Two tests, and what they separate:**

| Test | Tag | Purpose |
|---|---|---|
| calls another function through an opted-in frame | `[staging][libjit][abi][call]` | the frame form stages a local frame and returns 6; the register form still compiles the fixture |
| rejected module leaves the runtime usable | `[safety][instantiate][libjit]`, `WASMJIT_SAFETY_PROBES=1` | a rejected artifact must not poison the runtime |

### The theory that was wrong

I claimed `Runtime::instantiate` was not exception-safe, because two earlier attempts
both produced *two failures and a segfault in tests that never enable frame mode*.
The probe **disproves it**: after a hook throws, the next instantiation succeeds and
exports its function correctly.

What actually happened: **the appended test had landed inside another test's body**.
That produced a compile-time-valid but semantically broken test, whose failure and
follow-on crash looked like corrupted state. The same class of mistake had already
happened once -- in `AsmjitBackendTests.cpp`, inserted at line 20 through
`insert_line` mid-function.

**Rule (repeated, because it repeated):** a test that fails in a way that does not
match its own assertion is a **test-file** defect until proven otherwise. Check where
the test was inserted before hypothesising about the product.

That mistake cost two reverts and a wrong architectural claim. The claim is now
struck from the record, and section 12's reasoning -- "throwing mid-build corrupts a
half-built context" -- stands only as *plausible for the dispatcher variant*, not as
established fact.

## 14. Step 4b landed: LibJIT frame calls, imports and multi-result (2026-11-07)

The module-level refusal from section 13 is gone. The frame signature is the canonical
`void fn(VMContext*, void* frame)`; a frame call allocates a local call frame, stages
arguments at the shared `WASM::frameValueBytes` offsets, invokes `(ctx, frame)`, and
unpacks every result. Direct, indirect, `call_ref` and native-import calls share that
path, and a multi-result frame function writes its whole tuple into the caller's result
block.

The opt-in rule is unchanged: the register form is the default and the frame form is
only selected by `WASM::setFrameCallAbiOptIn(true)`.

**Limitation:** a frame `return_call` is a frame call plus a frame return. It is correct
but not constant-stack; the register trampoline remains the constant-stack path.

**Evidence:** frame call 6, native import 42, multi-result 7/9, 100-link frame tail 1,
multi-result `call_ref` 11; suite 22437 assertions in 820 cases, seed 12345.
