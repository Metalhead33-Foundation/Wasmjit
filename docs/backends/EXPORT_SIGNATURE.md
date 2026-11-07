# What the staging frame changes about exported signatures

**Status:** living note, 2026-11-06 (`backendsMh33`, commit `aa5fb47`).
**Companions:** [`STAGING_CALL_ABI.md`](STAGING_CALL_ABI.md) (the design and its
step scoreboard), [`LIBJIT_ARGUMENT_DEFECT.md`](LIBJIT_ARGUMENT_DEFECT.md) (the
measured loss), [`../ABI_PORTABILITY.md`](../ABI_PORTABILITY.md) (the ABI and the
GC object contract this note defers to).

This note answers one question an embedder asks the moment the frame form appears:
*what happens to the native C++ signature I write for a wasm function?* The short
answer is that **imports do not change at all, exports change only at the raw
seam, and the public embedding surface keeps its typed entry points.**

## 1. The three directions, side by side

| Direction | Today (register form) | Frame form |
|---|---|---|
| **Wasm → native import** | `R native(VMContext*, A, B, …)` | **unchanged** -- the trampoline passes typed arguments out of the operand stack |
| **Native → wasm export (raw)** | `R fn(VMContext*, A a, B b, …, [void* resultBuf])` | `void fn(VMContext*, void* frame)` |
| **Compiled → compiled** | per-backend spellings, with per-backend ceilings | one form, no ceilings |

So the change is confined to the two rows that involve a *compiled* callee. A
native function the embedder registers as an import keeps its exact signature.

## 2. Marshalling an export through the frame

The frame is a flat byte block with two regions in this order: the **argument
block**, then the **result block**. Widths come from `WasmBackendAbi.hpp`:

| Value kind | Bytes |
|---|---|
| `i32`, `f32`, `i64`, `f64`, any reference | 8 |
| `v128` | 16 |

* argument *i* starts at `frameArgumentOffset(kinds, i)`;
* results start at `frameArgumentsBytes(kinds)` and are addressed with
  `frameResultOffset(kinds, i)`;
* the caller allocates `frameBytes(argumentKinds, resultKinds)` bytes and
  **16-byte aligns** it, because `v128` must be 16-byte aligned.

Three consequences worth stating plainly:

1. **No arity limit.** SLJIT's four-argument ceiling, LibJIT's sixth-word loss and
   AsmJIT's vector-result special case are all properties of register passing.
2. **No result-buffer parameter.** A multi-result function's results simply occupy
   more of the result block, so the trailing `void*` disappears.
3. **One marshaller.** A caller that can write a frame can call any backend, which
   is what removes the per-backend knowledge from embedder code.

## 3. What does *not* change

* The context pointer is still the first argument, and still travels with the
  chain (`docs/ABI.md`).
* Reference ownership, finalization and GC object layout are untouched
  ([`../ABI_PORTABILITY.md`](../ABI_PORTABILITY.md) owns them).
* **The frame is not a GC-scanned root.** A reference held only in a live frame is
  invisible to a collection. Either keep referenced values in scanned storage
  across a safepoint, or register the frame; a backend must not keep the only
  reference to a GC object in an unregistered frame.

## 4. Cost, and therefore the default

Frame passing costs one store per argument at the caller and one load per
parameter in the callee's prologue. That is cheap but not free, so:

* **LibJIT** keeps the register form (its `v128` aggregate already delivers wide
  vector signatures) and takes the frame only on request;
* **SLJIT** needs it to exceed four argument entries, so it is the form a wide
  module gets;
* **AsmJIT** will follow its own measured ceiling (see §6).

The request is explicit: `WASM::setFrameCallAbiOptIn(true)`
(`WasmBase/WasmBackendAbi.hpp`). A backend must never *infer* the form from
signature measurements -- measured on LibJIT, inference broke ten working modules.

## 5. The public embedding surface keeps typed entry points

The frame is the **backend seam**, not the embedder's API. The wrapper described in
[`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) marshals typed arguments into a
frame internally, so a `WASM::Runtime` user writes and calls typed functions and
never sees a frame. Only code that talks to a backend directly -- a harness, a
second code generator, a test -- needs the frame bytes.

## 6. Cross-backend calls: legal, and still forbidden in production

The frame makes the ABI identical on every backend, so a call whose caller and
callee are compiled by different backends is no longer *ill-formed*. It stays
**forbidden in serious production code**, and the reason is empirical rather than
stylistic:

* each backend's frame path is pinned only by its **own** tests, so a cross-backend
  call combines two independently-verified halves into one **unverified whole**;
* a divergence in a corner -- a `v128` result, a multi-result block, an import
  trampoline -- would surface as memory corruption in a program that "passed" each
  backend's suite separately;
* there is no cross-backend test suite yet. Until there is (step 6's last gate),
  the rule is: **one backend per process, or per module graph.**

Treat it the way one treats mixing allocators: permitted by the type system,
refused by review. "Just because you can, doesn't mean you should."

## 7. AsmJIT (step 5): brief before code

AsmJIT differs from LibJIT in **motive**, and that changes the design:

* it has a **real ceiling** (`kArgumentSlotCeiling = 4`), so the frame is needed
  rather than optional, and it may be default-on where a signature exceeds it;
* the **`v128`-result special case retires** -- the concrete gain of step 5;
* call arguments must be built **before** the invoke node exists
  (`prepareCallArguments`); the earlier attempt set an argument after the node
  existed, which is the mistake to avoid.

### Verification checklist, applied in this order

1. **Find which signature overload `translateTypes` actually calls.** This cost two
   build cycles on LibJIT: the branch had been written into the `CoreTypeView`
   overload while the live path used the `FuncType` one. Settle it by reading, or
   by a unit test that asserts the translated signature's shape -- never by
   guessing.
2. **Aggregates are written through their address.** `jit_insn_store_relative` on a
   *value* segfaulted on LibJIT; AsmJIT has the same class of trap with `mem`
   operands versus value operands.
3. **Separate prologue faults from driver faults by calling the raw entry
   directly.** A hand-built frame plus a direct call to `rawFnPtr` is the cheapest
   discriminator there is.
4. **Write the two tests before the code**: the signature's shape, and a wide
   module returning 6 through the frame. Both are small, and both fail for the
   obvious wrong implementation.

Only then the five sites: signature, prologue, result path, call arguments,
`requireScalar` refusals.

## 8. Where each measured claim comes from

| Claim | Evidence |
|---|---|
| SLJIT caps at four argument entries | `function_check_arguments`, `sljitLir.c:924` |
| LibJIT loses the sixth argument word | [`LIBJIT_ARGUMENT_DEFECT.md`](LIBJIT_ARGUMENT_DEFECT.md) |
| A `v128` must be written through its address | measured on LibJIT, step 4a (segfault, then 6) |
| Inference is unsafe; opt-in is right | ten suite failures and a crash on LibJIT |
| The frame form works end to end on LibJIT | `v128_multi` -> 6 with the flag on |
| The frame form works end to end on SLJIT, calls included | `staging_call` -> 6 |


## 9. AsmJIT site map (read, not guessed -- 2026-11-06)

Checklist item 1, answered: `ModuleCompiler::translateTypes`
(`AsmjitModuleCompiler.cpp:4976`) builds every signature through the **static**
`TypeTranslator::translateFunctionType(type)` at line 4988 and stores it in
`internals.translatedTypes[i]`. The frame branch therefore belongs **there**, in
`AsmjitTypeTranslator.cpp` -- not in a member overload, and not in the module
compiler.

The call path is one straight line, which is why step 5 is smaller than LibJIT's:

| Line | Site | Change in frame mode |
|---|---|---|
| 1409 | `prepareCallArguments(calledType, arguments)` | build the callee frame in a stack buffer; the arguments become frame bytes |
| 1411 | `compiler.invoke(..., *signatures[typeIndex])` | the signature is already the frame one |
| 1413 | `invoke->set_arg(0, calleeContext)` | unchanged -- the context is still first |
| 1414 | `setCallArguments(invoke, preparedArguments)` | pass the buffer's **address** as argument 1, nothing else |
| 1415 | `emitCallResult(invoke, calledType, ...)` | the result is a frame read after the invoke, not a return value |

And the refusals step 5 retires -- note **where** they sit, because it is the
opposite of what one expects:

| Line | Refusal | Why it disappears |
|---|---|---|
| 892 | `requireScalar(parameter.val, "parameters")` -- **function entry** | a `v128` parameter arrives as 16 frame bytes |
| 897 | `requireScalar(result.val, "results")` -- function entry | a `v128` result is 16 result bytes, not a vector register |
| 1358 | `requireScalar(parameter.val, "indirect-call parameters")` | same, at the call site |

That is the whole of step 5's motive: AsmJIT refuses vectors *at the door*, so the
frame is what lets it stop refusing -- a real capability gain, unlike LibJIT where
the frame is a parity option.

**Not implemented.** The two tests of checklist item 4 come first: the translated
signature's shape, and a three-vector module returning 6 through the frame.

## 10. Step 5 design consequence: AsmJIT has no driver

`AsmjitModuleCompiler.cpp:5070` sets `rawFnPtr = fnPtr = functionRecords[i]->entry`,
so in AsmJIT the **exported entry is the compiled function itself**. LibJIT hides
the frame's result store in a generated driver; AsmJIT has nowhere to hide it.

Therefore step 5's signature is `(ctx, frame) -> void` and the **function writes
its own results** into the frame at `frame + frameArgumentsBytes`. Two consequences:

* **Every return site needs the frame store**, not just the epilogue: the normal
  epilogue (`:1014` onward), the trap path (`:948`-`:957`), and the explicit
  `return` the dispatcher emits. LibJIT needed one site because the driver
  collected the value; AsmJIT needs all of them, which is why step 5 is a larger
  unit than step 4a despite having fewer files.
* **`bufferedVectorResult` retires rather than adapts.** The current design threads
  a caller-supplied buffer through a trailing pointer argument (`:900`-`:910`).
  The frame *is* that buffer, so the special case disappears instead of being
  ported -- the capability gain the note promised for AsmJIT.

Prologue shape, for the same reason: the parameters arrive as frame words, so the
`set_arg(i + 1, …)` loop at `:960`-`:985` becomes a copy out of the frame, and the
`requireScalar` refusals at `:892` and `:1358` are skipped in frame mode rather
than relaxed.

**Not implemented.** Six edits across three files plus the return sites, with the
prologue and return paths of a register-allocating backend. The two tests come
first (signature shape; `v128_multi` returning 6 through the frame), then the
edits in this order: signature, prologue, entry refusals, return sites, call-site
refusals.

## 11. Step 5 split into subtasks, with the tests written first

The tests for step 5 now exist (`Test/AsmjitBackendTests.cpp`), in two kinds:

| Kind | Tag | State today |
|---|---|---|
| baseline pins | `[asmjit][abi][sig]`, `[asmjit][abi][v128][gap]` | green -- they describe the behaviour step 5 must change |
| step-5 gates | `[staging][asmjit][abi][frame]`, gated by `WASMJIT_STEP5_GATES=1` | red by design, failing with the exact refusal text |

Running them is one command: `WASMJIT_STEP5_GATES=1 ./Test/WasmJit '[staging][asmjit]'`. The
red run's message -- *"AsmJit supports only i32/i64/f32/f64 parameters"* -- is the entry
refusal at `AsmjitModuleCompiler.cpp:892`, which is precisely the thing subtask **5c** lifts.
So the gate does not merely fail; it **names the site**.

The baseline pins matter as much as the gates, and one of them is deliberately hostile: the
*refusal* is asserted, not tolerated. The day step 5 lands that test fails, forcing whoever
lands it to rewrite it into the positive assertion -- a gap cannot be closed silently.

### The subtasks, each with its own gate

| # | Subtask | Gate |
|---|---|---|
| **5a** | Transitive mode: `translateFunctionType` takes the frame setting (it is `static`, so it needs a parameter, not a member flag) | `[staging][asmjit][abi][sig]` once frame mode exists |
| **5b** | Signature: frame mode emits `(ctx, void*) -> void` | the signature-shape test, extended |
| **5c** | Prologue + entry refusals: parameters read from the frame; `requireScalar` at `:892`/`:1358` skipped in frame mode | "instantiates a v128-parameter module in frame mode" |
| **5d** | Return sites: results stored into the frame at `frame + frameArgumentsBytes`, `ret()` void -- epilogue `:1014`, trap path `:948`-`:957`, dispatcher `return` | "takes three vectors through a frame and returns 6" |
| **5e** | Call sites: `prepareCallArguments` builds the callee frame; `setCallArguments` passes its address (4b for SLJIT's analogue) | `staging_call` on asmjit |
| **5f** | `bufferedVectorResult` deletion, and the rewritten gap pin | the gap test inverted |

Each subtask is green-or-revert on its own. 5a is a signature change with no behaviour, 5b a
shape with no store, **5c is the first that can crash**, and 5d is the one that must touch
three return sites. That ordering exists so a failure localises to one subtask rather than to
"step 5".

## 12. Step 5 in progress: 5a/5b/5c landed, one blocker named (2026-11-06)

**Landed behind the opt-in flag** (default suite untouched at 21805 assertions):

* `translateTypes` decides the mode once, records it on the instance and passes it
  to the static translator;
* a vector parameter is **no longer refused at the door** in frame mode;
* the prologue captures `frame` as argument 1 and copies every parameter out of
  it -- 8 bytes each, 16 into a local buffer for a `v128`, which is the same
  buffer the register form already allocates for a vector local;
* the **epilogue** stores the single result into the frame at
  `frame + sum(param widths)` and emits `ret()`.

**The one blocker, measured:** finalisation still fails with AsmJIT error 25 for a
frame-mode function, and the cause is the **trap path**: it returns a value
(`ret(zero)`) under a signature that is now `void`. Same class of defect as the
epilogue, one site further on, and it must be patched the same way -- store the
zero into the frame or simply `ret()` -- before the gates can go green.

**Consequence for the subtask split:** 5b and 5c are **one unit**, not two. The
"instantiates a v128-parameter module" gate cannot pass before the return path is
consistent, because AsmJIT finalises every function at instantiation. The ladder in
section 11 is otherwise intact: 5d (calls) and 5e (`bufferedVectorResult`) are
untouched by this.

**Why committing with red gates is right here:** the gates are the feature's own
checklist (they skip unless `WASMJIT_STEP5_GATES=1`), the feature is opt-in, and the
default path is byte-for-byte unchanged. What would be wrong is enabling the switch
while the trap path disagrees with the signature.

## 13. Step 5d attempted: the frame call does not finalise (reverted)

The direct-call site was written in full -- frame allocated with
`compiler.new_stack(argBytes + 16, 16)`, arguments packed before the invoke node,
`set_arg(0, ctx)` and `set_arg(1, frameAddress)`, results read back at
`frame + argBytes` -- and the refusal relaxation that lets a vector argument reach
the call. **`v128_multi` still passed** (10 assertions in 3 test cases), so 5b/5c
survive; the new `staging_call` gate failed with

```
AsmJit compiler finalisation failed: 25
```

which is the same error code the trap path produced when a `ret` disagreed with the
signature. That narrows it to an **operand/type mismatch in the emitted
instructions**, and the three candidates worth checking first, in order:

1. **`asmjit::x86::Mem(frameAddress, offset, size)`** -- a sized memory operand built
   from a base and an offset. If v1.21 wants `.set_size()` or a `clone_adjusted()`
   form, the store size is unknown at validation time -- exactly error 25.
2. **The vector argument registers.** A `v128` value in `values` is a *buffer
   address*, so `qword_ptr(arguments[i], word * 8)` is right only if the buffer is
   the value; confirm that the same expression is used elsewhere for vectors.
3. **`set_arg` on a void-returning invoke.** The frame signature returns void, and
   the code sets two arguments; verify that `arg_count()` is 2 and not 3.

Everything was reverted, so the branch is back to `e492712`: suite 21805 in 759
cases, step-5 gates 10 in 3, all green.

**Method note.** This is the one place in step 5 where the earlier pattern did not
help: the gate failed with the same code as before but for a *new* reason, so the
message no longer named the site. The next attempt should isolate the suspect with a
**unit test of the emitted call** -- compile one function that calls a three-vector
callee and inspect `compiler.code()` -- rather than bisecting the generated function
against a gate.

## 14. Step 5d done: the deep look found the culprit outside the suspect list

The three suspects were investigated in order and **all three were innocent**, which
is itself the finding:

| # | Suspect | Verdict |
|---|---|---|
| 1 | `asmjit::x86::Mem(base, offset, size)` is not a valid sized operand | **cleared by evidence** -- the same form already works in the epilogue's frame result store, which passes |
| 2 | a `v128` value in `values` is not a buffer address | **cleared** -- `dispatchLocalGet` pushes exactly such an address for a vector local, and `copyVector` consumes it the same way |
| 3 | `set_arg` on a void-returning invoke | **cleared** -- the frame signature has two arguments and the code sets two |

**Suspect 4 -- the real one -- was found by following the call rather than the error
code.** `emitPendingTrapReturn()` is invoked by *every* call site, and it ends with
`ret(zero)` whenever `resultCount != 0`:

```cpp
if (frameMode || resultCount == 0)   // was: if (resultCount == 0)
```

In frame mode the signature returns void, so that `ret(zero)` is an operand mismatch
-- error 25 -- at **every call site**, not merely at the trap path fixed in 5c. That
is why `v128_multi` (no calls) passed while `staging_call` failed: the failing code
only exists in functions that call something.

### The reasoning that located it

The error code was identical to the trap-path failure fixed earlier. Two different
defects sharing one code means the code no longer names the site -- so the question
becomes *which code runs only in a function containing a call?* The answer is the
post-call trap check, and the answer arrived before any test was written.

**Rule added:** when an error code repeats, stop reading the code and start asking
**which code became reachable**. Reachability is the discriminator; the message no
longer is.

### Result

All four step-5 gates pass -- 14 assertions in 4 test cases -- including a compiled
function **calling another through a frame** and returning 6 on AsmJIT. The default
suite is 21806 assertions in 760 cases.

### Suspects that remain unexercised, and are worth a test later

* the `v128` **return** path through the frame (5e still carries
  `bufferedVectorResult`);
* indirect calls in frame mode -- the *direct* site is patched, the indirect one
  still uses `prepareCallArguments`;
* frame calls into a **native import**, where the callee expects the typed signature
  and must therefore be excluded from the frame path.

## 15. Step 5e done: the vector result crosses the frame

`bufferedVectorResult` was not deleted -- it still serves the **register** form,
which has no frame to put a vector result in. What 5e did instead:

* the flag is now `resultType == V128 && !frameAbi`, so the frame form never asks
  for the trailing caller buffer;
* the epilogue checks `frameMode` **before** `bufferedVectorResult`, because the
  buffered branch would otherwise win and ask for a buffer argument the frame
  signature does not have -- a branch *order* bug, invisible while every fixture
  had a scalar result;
* a `v128` result stores its **16 bytes** out of the local buffer into the frame,
  not the 8-byte buffer pointer, which is what the scalar path stores.

**Gate:** a module whose only export returns a `v128` (`asmjit_vec_result.wasm`)
instantiates in frame mode and yields lanes **7, 8, 9, 10** at offset zero. All five
step-5 gates pass -- 21 assertions in 5 test cases -- and the default suite is 21807
in 761 cases.

### What is left in step 5, stated exactly

| Item | Why it is safe to leave |
|---|---|
| `bufferedVectorResult` deletion | it is the register form's mechanism, and the register form is still the default for AsmJIT's narrow signatures |
| the `[asmjit][abi][v128][gap]` pin | still truthful: with the flag off a vector parameter is still refused |
| indirect calls in frame mode | the direct site is patched; an indirect frame call still takes the register path, which refuses vectors, so it fails loudly rather than silently |
| frame calls into a native import | must be *excluded* from the frame path, since an import expects the typed signature |

## 16. Register-form and frame-form can be bridged by a thunk (answer to a design question)

**Question.** Can a wrapper convert between a backend's register-based convention
(LibJIT today) and the frame-based one (mandatory for SLJIT and AsmJIT, opt-in for
LibJIT) -- for example with virtual registers on the frame or stack -- so that less
of LibJIT has to change, even at the cost of extra work on the other two?

**Answer: yes, and it is the better plan.** The frame form should be the **boundary**
ABI, not necessarily every backend's **internal** ABI.

### The shape

The adapter is generated **per signature, at the boundary**, never per call site:

| Crossing | Adapter |
|---|---|
| host -> wasm export | frame-shaped entry that unpacks the frame into typed arguments, calls the register-form function, packs the result back |
| wasm -> host import | the existing import trampoline, which already passes typed arguments |
| wasm -> wasm, same backend | whatever that backend is fastest at (frame for SLJIT/AsmJIT, register for LibJIT) |

The canonical contract stays exactly as this note defines it: **one way to expose a
C++ function to wasm, one way to call a compiled wasm function from C++, on every
backend.** What changes is only *where* the conversion happens.

### Why it is cheaper

* **LibJIT stops being a special case with unfinished work.** Its frame form is
  already proven (step 4a: arguments cross, results return 6), but flipping its
  whole call path (4b) is the expensive part. With an adapter, 4b is **not
  required** for the boundary contract: the driver -- which LibJIT already
generates per exported function -- becomes the packer, and nothing inside the body
  changes.
* **SLJIT and AsmJIT need no change at all** for exports, because their entry is
  already the frame form; their imports already use typed trampolines.
* **The correctness burden moves to one place per backend.** Packing order and
  widths (8 bytes per scalar, 16 per `v128`, results after arguments) get
  implemented once in the thunk instead of at every call site -- which is where
  every defect in this campaign actually occurred.

### Costs, stated honestly

* **One extra indirection per boundary crossing** (a call plus a copy of each
  argument), not per wasm-to-wasm call. For embedder-facing calls this is a fair
  price; for hot internal calls it is irrelevant, because those do not cross the
  boundary.
* **A second calling convention stays in the tree**, which the note elsewhere
  argues against. The mitigation is that it becomes **internal and unobservable**:
  no embedder and no cross-backend caller can see it, because the boundary always
  speaks frames.
* **Thunk caching must exist** for the cost to stay flat. The machinery is already
  there: `WASM::Callable`, `rawFnPtr`/`fnPtr`, LibJIT's drivers, and SLJIT's
  trampoline thunks are all per-signature entries that could carry the pack/unpack.

### Consequence for the plan

`P1` (multi-result through the frame) and `P2` (one spelling for exports and calls)
are **unchanged and still the deliverable** -- they are what the boundary contract
needs. What the adapter removes is **step 4b** (LibJIT frame-mode call sites), which
was the largest remaining block and existed only so that LibJIT could call itself in
a form nobody is obliged to see.

**Recommendation:** adopt the adapter. Implement it as P2's entry/exit thunk, and
retire 4b rather than finish it.

## 17. Correction: the wrapper belongs on the HOST side (embedder grammar stays typed)

Section 16 put the adapter between two conventions inside the backend. The project
owner corrected the seam, and the correction is right:

> The embedder should never see a frame. Both directions -- a host function called
> by the guest, and a guest function called by the host -- keep the typed
> `R f(VMContext*, args...)` grammar. Packing and unpacking is an *internal*
> concern of the backends that need frames (SLJIT, AsmJIT) and must be invisible.

### It is possible, and the machinery exists

`WASM::CallCache` is a **libffi-backed** per-export cache: it builds one `ffi_cif`
per callable whose `argTypes` already include the context pointer, and `invoke()`
takes pointers to Wasm arguments (`WasmBase/WasmCallCache.hpp`). libffi marshals
according to the **host C ABI**, whose argument capacity is far larger than any
backend's -- so the host side can carry the typed signature that the backend
cannot:

```
embedder:  invoke(entry, args...)          <- typed, unchanged, backend-agnostic
call cache: generic packer (host C++)      <- args -> frame using frameArgumentOffset()
backend:   void compiled(VMContext*, void* frame)
```

The packer is **generic, not per signature**: it loops over the cif's `argTypes` and
copies each argument into the frame at `frameArgumentOffset(kinds, i)`, reads the
result back from `frameArgumentsBytes()`. One implementation, no JIT change, no
template explosion.

### Why this is better than section 16

| | §16 (backend-side adapter) | §17 (host-side packer) |
|---|---|---|
| Embedder grammar | typed | **typed** (unchanged) |
| Guest -> host imports | unchanged | **unchanged** (already typed) |
| Extra JIT code | a thunk per signature | **none** |
| Work in SLJIT/AsmJIT | a new JIT-side adapter | **none** |
| Where frames appear | in the seam | **only inside a frame-mode backend** |

So the mandatory requirement -- one spelling to expose C++ to wasm, one spelling to
call compiled wasm from C++ -- is met **without asking any backend to change its
internal convention**. LibJIT keeps the register form and needs no thunk at all;
SLJIT and AsmJIT keep frames and the cache hides them.

### The one real piece of work it exposes

`v128` currently spells **differently per backend** at the boundary: LibJIT sets
`instance.setV128ParameterIsAggregate(true)` and the call cache describes that to
libffi, while the canonical rule is two `u64` words. That is a divergence the
embedder *can* see today. The uniform grammar requires the cache to advertise the
**canonical two-word** spelling always, with the packer converting for a backend that
spells it otherwise -- which is precisely this refactor's `D-L1` defect, now with a
home.

**Status:** §16's adapter is superseded for the boundary. It remains correct for a
different question -- a same-process, cross-backend *call* -- which is optional and
may stay forbidden.

## 18. P0b landed: a frame-mode AsmJIT module with imports is refused

```cpp
if (frameMode && !module.importFunctions.empty())
    throw std::runtime_error("AsmJIT frame ABI: calls into imports are not implemented");
```

in `ModuleCompiler::translateTypes`, before any code is generated. The reasoning is
the same as LibJIT's P0a, with one addition: a **wasm-to-wasm** import is not safe
either, because it is compiled by its own instance in whatever mode *that* instance
chose. Handing either kind `(ctx, frame)` would be a silent ABI mismatch.

**Evidence:** the whole suite stays at 21811 assertions in 763 cases and all five
step-5 gates still pass (21 in 5) -- no existing AsmJIT gate uses a module with
imports, which is exactly why the hole existed unnoticed.

### What is missing, stated rather than implied

**There is no gate test for P0b yet.** It needs a fixture with a *resolvable* import
(an unresolved one would throw from import resolution instead, and the test would
pass for the wrong reason). The honest gate is:

* a `.wat` with an import and a call to it, registered through a resolver;
* under the opt-in: instantiation throws with *this* message;
* without it: the module compiles and the import call works.

That is the first unit of the next session. Until it exists, P0b is a guard with no
proof -- better than a silent mismatch, but not yet a verified refusal.

### Where P0 stands now

| Subtask | State |
|---|---|
| P0a -- LibJIT refuses calls in frame mode | **done**, gated by `[staging][libjit][abi][call]` |
| P0a' -- instantiation failure leaves the runtime usable | **done**, gated by `WASMJIT_SAFETY_PROBES=1` |
| P0b -- AsmJIT refuses modules with imports in frame mode | **guard landed**, gate test pending |

## 19. P0b gated, and a caching caveat the gate exposed

`asmjit_import_call.wasm` (an import plus a call to it) now gates P0b:

* **default form:** instantiates, exports `caller`, and calling it returns **14** --
  the import really is reached with the typed signature `(ctx, i32) -> i32`;
* **frame form:** `REQUIRE_THROWS` -- the module-level refusal fires.

### The caveat: compiled code is cached per *module object*

The first version of the gate re-instantiated the **same** `module` and asserted a
throw. It did not throw, because the second instantiation reused the compiled code
from the first -- the mode was never re-decided.

That is worth stating generally, because it constrains the opt-in:

> `setFrameCallAbiOptIn` affects the **first compilation of a given module object
> only**. Toggling it afterwards and instantiating the same module again inherits
> the earlier mode.

Three consequences:

1. Tests must **reload** the module to exercise the other mode (what the gate now
   does).
2. An embedder that wants one ABI throughout should set the preference **before
   loading**, not before instantiating.
3. If the preference is ever meant to be per-instance rather than per-process, the
   compile cache must be **keyed by mode** -- a change worth making before the
   opt-in is advertised to embedders, and cheap while there is one way to set it.

**State:** suite 21816 assertions in 764 cases, `[staging][asmjit]` 14 in 6, all
green.

## 20. Is it a "virtual register file"? A precise answer

**Short answer: no, not in the compiler sense -- but yes in one specific, defensible
sense, and the distinction is worth keeping.**

### What a virtual register is

A virtual register is an **unbounded symbolic** register in an intermediate
representation: SSA values, liveness, coalescing, allocation to physical registers,
and spills decided by a register allocator. Nothing in the frame form is that:
nothing is allocated, nothing is live-range analysed, and no pass chooses between a
register and a slot.

### What the frame form actually is

A **memory-based calling convention**: a fixed-layout parameter-and-result block
the caller fills and the callee reads. The classical names are the *argument home
area*, a *spill file*, or a *static parameter block* -- Win64's shadow space is the
familiar cousin.

| Backend | Convention | Why |
|---|---|---|
| LibJIT | native C ABI (`jit_abi_cdecl`) | libjit *has* a register allocator, so the sixth-argument defect is a bug to fix, not a design limit to route around |
| SLJIT | fixed block | "stack-less" by design: no argument declarations without saved registers, and a hard four-entry cap |
| AsmJIT | fixed block | a four-entry cap plus a vector-register result special case |

### The one sense in which "virtual register file" is right

The block is **addressable storage for the call's value set**: a fixed-stride array
of slots (8 bytes each, 16 for `v128`) where every parameter has a *nominal index*
and the callee materialises each slot into a real register exactly when it needs it.
Arity is therefore bounded by **memory**, not by the machine's register file -- which
is precisely the property the two backends lack.

So the honest phrase is **"a register file mapped into memory"** -- a *virtual
register file* -- rather than "virtual registers". The adjectives do different work:
`virtual` because it is unbounded and nominal; `file` because it is an indexed
array; `memory` because there is no allocator behind it.

### Why the distinction is not pedantry

Calling it "virtual registers" would imply an IR that could be **optimised and
allocated** -- and there is none, deliberately. SLJIT and AsmJIT exist to be small
and predictable; adding an allocator to save eight bytes per call would be the wrong
trade. The frame is a **contract between caller and callee**, not a program
representation, which is also why it could be defined in `WasmBackendAbi.hpp` with
pure arithmetic and why every backend can honour it without sharing any IR.

### A detail this check turned up

`WasmCallCache` already maps `v128` to an **opaque pointer-sized cell** at the
`Callable` ABI (`ffiTypeFor`), not to two words. So the embedder-visible spelling
today is *pointer to 16 bytes*, and the canonical two-word rule applies to the
**frame**, not to the ffi boundary. The packer of section 17 must therefore
**dereference** the cell and copy 16 bytes into the frame -- a smaller change than
"change the spelling", and one that leaves every `Callable` signature untouched.

## 21. The cache packer: design settled, two concrete blockers found

Reading `WasmCallCache` end to end makes the packer a contained change inside
`ModuleCallCache::call`, and turned up two details that must be handled rather than
discovered later.

### The shape

```cpp
if (instance_->frameCallAbi()) {
    alignas(16) std::uint8_t frame[256];          // bounded: refused above 256 bytes
    std::memset(frame, 0, sizeof frame);
    for (i in params)
        copy args[i] into frame + frameArgumentOffset(kinds, i)   // 4 or 8 bytes
    reinterpret_cast<void(*)(void*, void*)>(entry.callable.fnPtr)(context, frame);
    copy frame + frameArgumentsBytes(kinds) into *result          // 4 or 8 bytes
    return;
}
```

Everything it needs exists: `frameArgumentOffset`, `frameArgumentsBytes`,
`frameValueBytes`, `valueKindOf`, `entry.paramKinds`, and `instance_->frameCallAbi()`.
And `instance_->context()` already supplies the first argument.

### Blocker 1: the result kind is not in the entry

`ModuleFfiEntry` carries `paramKinds` but **not** the result kind -- it only keeps
`returnType` (an `ffi_type*`), which cannot tell a 4-byte `i32` from a 4-byte `f32`,
nor a 16-byte vector from anything else. The packer needs the **Wasm** result kind to
know how many bytes to copy and where the result block starts.

Fix: add `WASM::ValueTypeCode resultKind` (and a `resultCount`) to `ModuleFfiEntry`
and set it where `entry.returnType` is chosen. Small, and useful to any future
caller.

### Blocker 2: the scalar copy width is not the slot width

A frame slot is always 8 bytes, but an `i32`/`f32` argument points at **4** bytes.
Copying 8 would read past the embedder's value. So the packer must
*zero the slot, then copy the value's own width* -- never `memcpy(8)`. The same
applies to the result. This is exactly the class of defect that produced the
`v128` pointer-vs-16-bytes confusion in step 5e, so it is worth a test of its own:
a cache call whose result is `i32` and whose neighbouring stack bytes are non-zero.

### Why this was not implemented in this session

The change is three edits across two files (header field, constructor, `call`) plus
the packer body and a test. Attempting it with too little budget left would risk
exactly the half-wired state the campaign has twice refused. The design above is
complete enough that the next session's first action is an edit, not a reading.

**Note also:** the `v128` case is already correct by construction -- the ffi boundary
hands a *pointer* to 16 bytes, so the packer copies 16 bytes from `args[i]` into a
16-byte slot, which is the canonical frame spelling. That is the one place where the
canonical rule and the ffi spelling agree, and it is worth asserting in the test.

## 22. The cache packer is implemented: the embedder never sees a frame

Three edits, ~30 lines, **no backend change**:

1. `ModuleFfiEntry` gained `resultKind` and `resultCount` -- an `ffi_type*` cannot
   distinguish a 4-byte `i32` from a 4-byte `f32`, and the packer needs the *Wasm*
   kind to know how much to copy (section 21's blocker 1);
2. `ModuleCallCache`'s constructor records both from the canonical view;
3. `ModuleCallCache::call` gained a frame branch that, when the instance says
   `frameCallAbi()`, packs the arguments into a 512-byte aligned block, calls
   `void(ctx, frame)`, and copies the result back out.

**The two blockers from section 21 are both handled in the code**:

* the **slot-versus-value width** trap -- a slot is 8 bytes, an `i32` argument points
  at 4, so the packer copies the value's own width into an already-zeroed slot and
  never a blind 8 bytes;
* the **`v128`** case is correct by construction: the ffi boundary hands a pointer to
  16 bytes, so the packer copies 16 bytes into a 16-byte slot, which is the
  canonical frame spelling.

### Proof

```
The call cache hides a backend frame behind the typed grammar
[staging][callcache][abi]   ->  3 assertions, green
```

The test instantiates `v128_multi` on **SLJIT**, asserts `frameCallAbi()` is true (six
argument words, so the backend is obliged to switch), then calls
`cache.call<std::int32_t>("three_lanes", lanes[0], lanes[1], lanes[2])` -- the typed
grammar -- and reads **6**.

That is the mandatory requirement demonstrated end to end: **one spelling to call a
compiled wasm function from C++, on any backend**, with the frame existing only
inside the backend that needed it.

### What this retires

* Section 16's per-signature JIT adapter: **not needed**.
* LibJIT step 4b: **already retired**, and now formally pointless -- its register
  form is hidden by the same packer.
* The ``"one marshalling path"`` goal for the **export** direction: **done** in code.

### What is still missing

* no test yet for a frame-mode instance on **AsmJIT** through the cache, which would
  exercise the vector-result path end to end (the `v128` result is stored in the
  frame by the backend and copied out by `widthOf(V128) == 16`);
* `frame[512]` is a fixed local bound with no refusal above it -- a signature larger
  than 512 bytes would overrun. It must either be sized from `frameBytes()` or
  guarded by a refusal. **This is a real defect, not a note**, and it is the first
  item of the next session.

## 23. The bound is guarded, and the vector result works through the cache

**Section 22's defect is closed.** `ModuleFfiEntry` now carries `frameBytes`
(computed once in the constructor: 8 per scalar slot, 16 per vector, arguments then
results), and the packer refuses a signature that does not fit its buffer instead of
overrunning it:

```
module call cache: signature needs N frame bytes, more than the 512 the packer holds
```

That is a refusal, not a fix: the limit is still 512 bytes. Lifting it means sizing
the block from `frameBytes` -- a heap block per call, or a cache-owned scratch buffer
reused across calls. Worth doing when a signature that wide appears; the guard makes
the current limit **safe** rather than merely unstated.

### The vector result through the cache

```
The call cache copies a v128 result out of a backend frame
[staging][callcache][abi][v128]   ->  green
```

`asmjit_vec_result` on **AsmJIT** under the opt-in: the entry reports
`resultCount == 1` and `frameBytes == 16`, the backend stores 16 bytes into its frame,
and the cache copies 16 bytes out -- lanes **7, 8, 9, 10** arrive intact.

That closes the loop the campaign opened: a `v128` used to be three separate special
cases (SLJIT's ceiling, LibJIT's aggregate spelling, AsmJIT's buffered result). It is
now **one typed call** from an embedder on two of three backends, with no special
case visible anywhere above the cache.

### Cache status

| | |
|---|---|
| SLJIT, six argument words -> 6 | **green** |
| AsmJIT, `v128` result -> 16 bytes | **green** |
| bound guard | **green** (refuses above 512 bytes) |
| LibJIT through the cache | **not tested** -- its register form should need none of this, which is exactly why it deserves the test |
| multi-result through the frame | not started |

## 24. All three backends answer the same typed call (the requirement, met)

```
The call cache reaches a LibJIT register-form export
[staging][callcache][abi][libjit]   ->  green
```

`v128_multi` on **LibJIT**, no opt-in, `frameCallAbi()` asserted **false** and
`v128ParameterIsAggregate()` asserted **true** -- and the very same typed call:

```cpp
cache.call<std::int32_t>("three_lanes", lanes[0], lanes[1], lanes[2]);  // == 6
```

The three cache tests now cover the three possible situations:

| Backend | Internally | Through the cache |
|---|---|---|
| SLJIT | frame (six argument words force it) | **6** |
| AsmJIT | frame (`v128` result) | **lanes 7,8,9,10** |
| LibJIT | register form, aggregates | **6** |

**That is the mandatory requirement, demonstrated:** one spelling to call a compiled
wasm function from C++, on every backend, with the frame -- and even LibJIT's
aggregate spelling -- invisible above the cache.

### A real bug found on the way

`ModuleCallCache::call` built its argument vector by **splitting every vector into
two word pointers**, which is right for the canonical two-word spelling and **wrong**
for LibJIT: its cif describes one 16-byte aggregate, so the vector must be passed as
**one pointer to 16 bytes**. The old code wrote two entries into a vector sized for
one -- a heap overrun that only needed a LibJIT vector call to happen.

No test had ever called a LibJIT export with a vector parameter *through the cache*,
which is exactly why it survived: the ffi path was exercised with scalars only.

**Rule, third time in this campaign:** the untested combination is where the bug
lives -- not the untested function. Here every function was tested; the *pair*
(backend, calling path) was not.

## 25. The trailing result buffer (MV5): what it is, and what multi-result does to it

**The question:** LibJIT implements multi-result by appending a `void*` result buffer
to the signature and returning void. What becomes of that, and does it apply to the
frame-based backends?

### Why it exists

It is a **LibJIT-specific** workaround with a documented cause: libjit's
aggregate-return ABI reads a hidden result-buffer pointer from an **unwritten frame
slot** when the callee has parameters, so returning a result tuple by value crashed
(`segfault at 8` or a general-protection fault) on every JIT-to-JIT call in
`call.wast`, `if.wast` and `fac.wast`. The fix was to pass the buffer explicitly as a
trailing pointer and return void (`docs/proposals/MULTI_VALUE.md` MV5).

### Does it apply to SLJIT and AsmJIT?

**No, and it never did.** Those backends have no aggregate-return ABI to work around.
Today:

| Backend | Multi-result today |
|---|---|
| LibJIT | MV5 trailing buffer, **necessary** for the register form |
| SLJIT | nothing special -- the frame's result block is already N slots |
| AsmJIT | **refused outright** (`AsmjitModuleCompiler.cpp:891`), plus a second refusal for block multi-values (`:4576`) |

So MV5 is not a portable convention that the frame improves; it is a scar from one
backend's calling convention, and the frame form makes it **unnecessary rather than
applicable**: `frameArgumentsBytes()` to `frameBytes()` is a contiguous block of N
result slots, so multi-result is *the same mechanism as one result, with a bigger
block*.

### What multi-result through the frame requires, per site

| Backend | Site | Change |
|---|---|---|
| AsmJIT | `:891` | refuse only when **not** in frame mode |
| AsmJIT | `:1056` (`values.size() != 1`) | frame mode accepts `values.size() == resultCount`, storing each at `frame + argBytes + frameResultOffset(i)` |
| AsmJIT | `:4576` | block multi-values stay refused -- out of scope |
| LibJIT | `emitFunctionReturn` | frame branch storing each result at its offset; the trailing-buffer path stays for the register form |
| Cache | `call` | loop `resultCount`, copying each result at `frameResultOffset` |
| `ModuleFfiEntry` | -- | `frameBytes` must count **all** result slots, not just one |

### The honest verdict on MV5

It **survives**, but its scope shrinks to LibJIT's register form -- which is now an
implementation detail, not a boundary anyone sees. If LibJIT were ever to be
frame-only, MV5 could be deleted; until then it stays, and no other backend should
ever acquire it.

**Not implemented in this session.** The AsmJIT change is not a one-liner: the return
path currently asserts exactly one value (`:1056`) and the operand stack is unwound
for one result, so multi-result touches the epilogue's value handling, not just a
store. That is the first unit of the next session, with the sites named above.

## 26. The frame is sized from the signature (item 2 closed)

The 512-byte refusal of section 23 is gone. The packer now uses:

* a **512-byte aligned inline block** for ordinary signatures -- no allocation on the
  path an embedder actually takes;
* an **exactly sized heap frame** (`frameBytes + 16`) above that, so a wide signature
  is served rather than refused. `operator new[]` returns storage aligned for
  `max_align_t` (16 on every supported target), which is what a `v128` slot needs, and
  the extra 16 bytes keep the arithmetic in `frameResultOffset` in range.

Suite 21832 assertions in 767 cases, cache tests 16 in 3 -- unchanged, because every
current fixture fits the inline block.

**What is not tested:** the heap branch. It needs a fixture with more than 512 bytes
of arguments -- 33 `v128` parameters is the cheapest shape -- and until that fixture
exists the branch is **code without coverage**. That is a deliberate, stated gap
rather than a hidden one: the inline path is exercised by all three backend tests,
the heap path by none.

### Remaining, in order

| # | Item | Size |
|---|---|---|
| 1 | multi-result through the frame | large: AsmJIT epilogue value handling (`:1056`), LibJIT `emitFunctionReturn`, cache loop, `frameBytes` over N results |
| 2 | heap-frame fixture (>512 bytes of arguments) | small: one `.wat` with 33 vectors, one test |
| 3 | compile cache keyed by mode | medium, and it makes the opt-in honest (section 19) |
| 4 | doc reconciliation | small: section 16 is superseded by 17/20/24; `D-L1` re-scopes to a note about *where* the canonical rule applies |

## 27. The heap branch cannot be reached yet: SLJIT has a parameter-count limit

Two attempts to exercise the heap frame failed, and the failures are the finding.

**Attempt 1 -- 33 `v128` parameters (536 frame bytes).** `frameBytes` came out as
**8**: the computation ran *before* `paramKinds` was filled, read an empty vector, and
sized every frame from the result alone. A genuine bug, fixed by moving the block
after the parameter loop; the three cache tests had passed regardless because none of
them looked at `frameBytes`.

**Attempt 2 -- 33 vectors, then 65 scalars (528 frame bytes), both correct after the
fix.** `ModuleCallCache::call` threw *"invalid entry"*: the exported `Callable` has
**`fnPtr == nullptr`**. Instantiation succeeds and `frameCallAbi()` is true, so the
module compiled -- but SLJIT produced no entry for the function.

That is a **SLJIT limit on the parameter count itself**, not on the register form: the
frame bypasses the four-*entry* ceiling, and something else (most likely per-function
frame-slot allocation or the parameter loop) refuses this width silently.

### Consequences

* the heap branch of the packer remains **code without coverage** -- now with a
  *reason*, not just a note: no backend tested here can produce a signature wide
  enough to reach it;
* the real limit should be **named and refused loudly** rather than surfacing as a
  null `fnPtr` an embedder has to discover. That is the next step, and it belongs in
  SLJIT's frame-mode refusal path, next to `kArgumentSlotCeiling`;
* the fixture and its test were removed rather than left red.

**Landed:** the `frameBytes` ordering fix, verified by the whole suite (21832 in 767)
and the three cache tests (16 in 3).

## 28. Section 27's conclusion was wrong: the limit was OURS

Section 27 blamed SLJIT for a parameter-count limit. It is not SLJIT's. In
`Sljit/SljitOpcodeDispatcher.cpp` there was an **unconditional** guard, older than the
frame form:

```cpp
if (parameterCount_ > kMaxWasmParameters) {      // kMaxWasmParameters = 3
    unsupportedReason_ = "the SLJIT backend supports at most three parameters";
    return;
}
```

It marked every frame-mode function with four or more parameters *unsupported*, so no
compiled function was created and the export had a null `fnPtr` -- which is exactly
the symptom section 27 misread as a backend limit.

**The frame really does bypass it**, which is what it was built for. The guard now
reads `if (!frameAbi_ && parameterCount_ > kMaxWasmParameters)`, matching the shape of
the argument-ceiling check at `:1118`.

### Proof, and the heap branch finally covered

```
The call cache sizes a frame past its inline block   [staging][callcache][abi][wide]
->  21 assertions in 4 test cases, green
```

65 scalar parameters are 520 bytes of arguments; `frameBytes` reports **528** (65 slots
plus the result slot), which exceeds the 512-byte inline block, so the **heap frame**
is exercised -- and the call returns **6**. The packer's heap branch is no longer code
without coverage.

### The lesson, which is the third of its kind

Section 26 assumed the packer's limit was reached only by huge signatures; section 27
assumed a null `fnPtr` meant a backend limit. **Both times the obstacle was our own
code**, and both times one grep found it. The habit that would have saved a round
each time: before blaming the dependency, grep the backend for its *own* constants --
`kMaxWasmParameters`, `kArgumentSlotCeiling` -- and check whether they are gated on the
mode.

## 29. Multi-result: the entry refusal is gated, the epilogue is pinned

**Done this round:**

```cpp
if (type.results.size() > 1 && !frameMode)        // was: unconditional
    throw std::runtime_error("AsmJit initial compiler does not support multi-value results");
```

The same shape as `kMaxWasmParameters`: the refusal was about the **register form**
and is now gated on the mode. It also taught a small lesson in ordering -- the local
`frameAbi` is declared further down, near the prologue, so the guard must use the
**member** `frameMode`; the compiler caught it in one build.

**Also found: a dead duplicate branch.** The epilogue has *two* `else if (frameMode)`
branches in the same chain, the second unreachable. It is an artifact of the 5e patch
inserting the frame branch before `bufferedVectorResult` while the earlier one stayed
behind. Harmless today, and **removed as part of the multi-result edit** rather than
left for a reader to trip over.

### The gap pin

```
AsmJIT still refuses multi-result in the epilogue   [staging][asmjit][abi][multi]
->  green (WASMJIT_STEP5_GATES=1)
```

A two-result fixture now reaches the epilogue and is refused there, because the
epilogue still unwinds exactly one value. The pin asserts the **current** refusal, so
the day multi-result lands it fails and forces the positive test to be written.

### What multi-result still needs, exactly

| Site | Change |
|---|---|
| `:1056` | accept `values.size() == resultCount` in frame mode |
| frame branch (A) | store each result at `frame + argBytes + offset(i)`, 8 bytes per scalar slot, 16 for a `v128` taken from its buffer |
| dead branch (B) | delete |
| cache `call` | loop `resultCount`, copying each at its offset |
| `frameBytes` | already counts one result slot; extend to all of them |

Suite 21838 assertions in 769 cases; step-5 gates 28 in 7.

## 30. Multi-result through the frame works on AsmJIT

```
AsmJIT stores two results in a frame   [staging][asmjit][abi][multi]   ->  green
```

A `(result i32 i64)` function under the opt-in: `7` arrives at frame offset **0** and
`9` at offset **8**, because the result block starts at `frameArgumentsBytes` (zero
parameters here) and each scalar takes an 8-byte slot.

Three edits made it work, all of them **additive**:

1. the arity check became `values.size() != (frameMode ? resultCount : 1)`;
2. a new branch `frameMode && resultCount > 1` stores every result in declaration
   order, 8 bytes per scalar and 16 per `v128` taken from its buffer;
3. the entry refusal at `:890` is gated on the mode (section 29).

The existing single-result frame branch was left untouched, which is why the change
was additive: a new branch *before* it, rather than a rewrite of it.

### The gap pin did its job

The previous round's pin asserted *"AsmJIT still refuses multi-result in the epilogue"*.
When the epilogue learned to store N results, that pin **failed** -- and rewriting it
into the positive test above is exactly the workflow the pin exists to force. A gap
cannot close silently.

### What multi-result still needs

| Site | Change |
|---|---|
| cache `callWithResults` | frame-aware: copy `resultCount` results out of the frame instead of passing a buffer |
| cache `frameBytes` | count **all** result slots, not just one |
| dead branch (B) | still there, still unreachable (section 29) |
| LibJIT | `emitFunctionReturn` needs the same frame branch; MV5 stays for its register form |
| SLJIT | already emits multi-result from the frame (its `allReturns` path); **untested under the opt-in** |

Suite 21838 assertions in 769 cases, step-5 gates 31 in 7.

## 31. Two results through the cache: the invariant, held

```
The call cache returns two results without exposing the frame
[staging][callcache][abi][multi]   ->  green
```

A `(result i32 i64)` export on **AsmJIT** under the opt-in: the entry reports
`resultCount == 2` and `frameBytes == 16` (two slots, no parameters), the backend
stores both results into its frame, and the cache copies them into the caller's
typed buffer as **7** then **9**.

The embedder supplies a buffer and receives typed values. Nothing above the cache
mentions a frame, a slot or a backend -- which is the invariant as stated:

> The embedder only ever sees typed, non-frame signatures in both directions.
> What happens underneath is the backend's business.

### The layout decision this forced

The frame path writes results as **slots in declaration order** (8 bytes per scalar,
16 per `v128`), and the caller's buffer uses the *same* layout. That is deliberate:
the buffer layout must be backend-independent, or the embedder could tell which
backend ran -- the exact leak this design exists to prevent.

Consequence: process-wide invariants now are `frameArgumentsBytes` for arguments and
slot order for results -- and that single rule covers every backend, including the
register form, because the *cache* is what converts.

### What remains

| Site | Change |
|---|---|
| LibJIT `emitFunctionReturn` | frame branch for N results; MV5 stays for the register form |
| SLJIT multi-result under the opt-in | already emits from the frame; **untested** |
| dead branch (B) in the AsmJIT epilogue | unreachable; delete on the next touch |
| compile cache keyed by mode | section 19; makes the opt-in honest |
| doc reconciliation | section 16 is superseded by 17/20/24 |

Suite 21838 assertions in 769 cases; cache tests 28 in 5.

## 32. SLJIT's multi-result frame path TRAPS (a claim corrected by a test)

Section 31 said SLJIT "already emits multi-result from the frame; untested". That was
wrong, and the test written to confirm it says so:

```
SLJIT stores two results in a frame for a wide signature
->  IOT instruction (SIGILL), 3 of 4 cases passed
```

The fixture is the only shape that reaches SLJIT's frame form *with* more than one
result: three `v128` parameters (48 bytes, so the frame is forced) and
`(result i32 i64)`. It traps.

### Why this was predictable in hindsight

SLJIT handles multiple results in the **register** form through its `allReturns`
mechanism, which returns them in registers. In frame form the results must be
**stored into the frame's result block**, and that code does not exist -- the callee
falls through to a return path that was never given the frame.

So the correct status is not "untested" but **"not implemented"**, and the difference
matters: an untested path might work; this one traps deterministically.

### What this means for the mandatory requirement

The requirement -- one typed way to call compiled wasm, every backend -- is **met for
single results on all three backends** and for **multi-result on AsmJIT**. SLJIT's
multi-result frame form is a gap, and it is now a *known-broken* gap rather than an
assumed-working one.

**Interim rule, worth stating:** a wide signature with more than one result is
supported on AsmJIT only. SLJIT refuses nothing here -- it traps -- so until the store
is implemented, SLJIT should **refuse** such signatures rather than miscompile. That
refusal is a one-line guard in the same place as `kMaxWasmParameters`, and it is the
next unit.

Suite 21845 assertions in 770 cases, green; the fixture and test were removed rather
than left red.

## 33. Section 32's "next unit" already exists: SLJIT refuses multi-result at line 958

```cpp
if (prepared.allReturns.size() > 1) {
    unsupportedReason_ = "the SLJIT backend does not support multiple results";
    return;
}
```

Unconditional, in the same function as the parameter guard, **older than the frame
form**. So there is no missing refusal to add, and section 32's "one-line guard, next
unit" was wrong on its second point as well as its first.

### What the trap actually was

An **unsupported function is still exposed as a callable**, and calling it lands in a
trap stub -- the IOT instruction the test hit. That is deliberate for a *wasm-level*
trap, but it is the wrong signal for "this function was never compiled":

* an embedder cannot distinguish "your module called `unreachable`" from "this backend
  cannot compile this shape";
* a `nullptr` entry (the 33-parameter case) and a trap stub (the multi-result case) are
  two different spellings of the same fact, neither of them diagnosable;
* `docs/SAFETY.md` prefers a diagnosable failure to a process trap.

### The real remaining work, restated

| # | Item | Where |
|---|---|---|
| 1 | unsupported functions should not be handed out as callables -- null `fnPtr` plus a queryable reason, or `nullopt` from `exportedFunction` | core instantiation / `ModuleInstance::exportedFunction`, **not** a backend |
| 2 | SLJIT stores N results into the frame (the actual capability) | `SljitOpcodeDispatcher` return path |
| 3 | LibJIT `emitFunctionReturn` frame branch | LibJIT, register form stays the default |
| 4 | delete the dead epilogue branch, cache keyed by mode, doc reconciliation | housekeeping |

Item 1 is worth doing **before** item 2 despite being "just reporting": every remaining
unsupported shape (LibJIT frame calls, AsmJIT imports in frame mode, SLJIT
multi-result) currently surfaces through one of those two indistinguishable signals.

Suite 21845 assertions in 770 cases, green.

## 34. Item 1 attempted and refuted in one run: a null `fnPtr` is ambiguous BOTH ways

The proposed fix was to stop handing out unsupported functions as callables: in
`ModuleInstance::exportedFunction`, return `nullopt` when the internal callable's
`fnPtr` is null.

**Three tests failed immediately**:

```
Test/SljitBackendTests.cpp:1064   REQUIRE( copy.has_value() )
Test/RuntimeTests.cpp:423         REQUIRE( instance.exportedFunction("answer").has_value() )
Test/PluginTests.cpp:257          REQUIRE( instance.exportedFunction("answer").has_value() )
```

So `fnPtr == nullptr` does **not** mean "the backend could not compile this": it also
means "this backend provides an instance without compiled entries", which is a
legitimate configuration that Runtime and Plugin tests exercise.

### The finding, which is worth more than the fix would have been

A null `fnPtr` is ambiguous **in both directions**:

* it can mean *unsupported shape* (SLJIT's 33-parameter function);
* it can mean *a backend with no compiled entries at all* (a legitimate instance).

Inferring support from it is therefore impossible. The signal has to be **explicit**:

| Option | Cost |
|---|---|
| a `supported` flag beside `fnPtr` in `Callable` | changes a frozen struct -- needs an API decision, `docs/API_ABI_STABILITY.md` territory |
| a `ModuleInstance::unsupportedReason(name)` query | additive, no struct change, and it can carry the backend's own text ("the SLJIT backend does not support multiple results") |
| `nullopt` from `exportedFunction` | **refuted** -- it breaks legitimate instances |

The middle option is the right one, and it needs the backend's `unsupportedReason_` to
survive into the instance rather than dying in the dispatcher.

### Fourth instance of the same pattern

Section 10 named three. This is a fourth: I assumed what a null meant, and the suite
refuted it in one run. The rule holds and is sharpened: **a sentinel already in the
tree has a meaning, and it is whatever the tree says -- not what the new code needs it
to be.**

Suite restored to 21845 assertions in 770 cases, green; the change was reverted.
