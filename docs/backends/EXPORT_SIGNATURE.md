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
