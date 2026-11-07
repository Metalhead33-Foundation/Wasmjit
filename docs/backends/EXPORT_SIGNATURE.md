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
