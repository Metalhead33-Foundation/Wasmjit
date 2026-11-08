# Remaining-work post-mortem (2026-11-07)

This note is the mandatory record of the "what remains" work, item by item, in
the order of `HANDOFF.md` section 7. Each section records what was intended, what
actually happened, the root cause of every surprise, and what is still open.

## Status at the end of this pass

| # | Item | Status | Evidence / blocker |
|---|---|---|---|
| 1 | constant-stack tail calls | **done for the register/frame tails that use the driver** | self, direct, indirect and frame (scalar and v128) chains plus `call_ref`/`return_call_ref` (register, frame, v128 and multi-result) go through the driver (`TAILSELF`/`TAILMUTUAL`/`TAILINDIRECT`/`TAILFRAME`/`TAILV128FRAME`/`TAILREF`/`TAILREFFRAME`/`TAILV128REFFRAME`/`TAILREFMULTI`/`TAILINDIRECTFRAME`, 1,000,000 links, `run()=1`); LibJIT frame tails are call+return, not constant-stack |
| 2 | shared reference/table atomics | **done** | numeric, struct/array reference, shared-table and global `eqref` atomics run on all three (`SHAREDREF`, `TABLEATOMIC`, `GLOBALREF`); v128 atomic slots are rejected by validation (`V128ATOMIC`) because the proposal does not allow them |
| 3 | block parameters/results (scalar and v128) | **done** | `BLOCKPARAM` block=42 if=11/12 loop=0; `BLOCKV128` block=42 if=7/8 loop=7; `BLOCKRESV128` 5,6,7/8,9 |
| 4 | bulk memory on memory64 | **done** | `MEM64BULK` copy=97 fill=65 init=97 on all three |
| 5 | exceptions and catch payloads | **done** | local/cross-instance catch, `catch_ref`, multi-value payloads and a catch payload routed to a loop parameter (`CATCHLOOP` 42) |
| 6 | LibJIT frame form | **done** | frame call 6, import 42, multi-result 7/9, frame tail 1, multi-result `call_ref` 11; the form remains opt-in and default-off |

Suite at the last commit: **22445 assertions in 821 cases** under
`--rng-seed 12345`, green; `[headerdoc]`, `[docs]`, `doxygen` and `abi-check` are clean.
The curated capability ledger has no open opcode/ABI row. That is not a
production verdict: the generic runs for SLJIT and AsmJIT fail and crash, so
only LibJIT is production-ready. See
[`PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md). The frame form on LibJIT
stays opt-in, and its frame tails are call+return rather than constant-stack.

## Item 1 — constant-stack tail calls

### Intended

Replace the `call` + `return` lowering of `return_call` with a real trampoline on
SLJIT and AsmJIT, using LibJIT's model: a per-function driver (exposed as
`Callable::fnPtr`, raw body in `rawFnPtr`) and a uniform per-type thunk
(`Callable::tailEntry`) that re-enters the next link without growing the native
stack.

### What we actually did (first slice)

**Direct self-recursive tail calls are now a loop on all three backends.** The
backend stores the new arguments into the parameter locals, resets the
non-parameter locals to zero (a tail call is a fresh invocation), and branches
to a body-entry label placed after the prologue. The frame is reused, so a
self-recursive chain runs in constant native stack.

Evidence (`tailcall_self_probe.wat`, 1,000,000 recursive links):

```
TAILSELF libjit: run()=0
TAILSELF asmjit: run()=0
TAILSELF sljit: run()=0
```

With the old call+return path this fixture hits the guest stack guard
(`CallStackExhausted`); the probe is therefore a real property, not a shape
check.

### SLJIT implementation notes

* A `bodyStart_` label is emitted at the **end** of `beginFunction`, after the
  context/trap-slot resolution and the local initialisation. A self tail call
  must skip the stack guard (the frame is the same) and must not re-run the
  prologue's parameter unpacking.
* The argument slots are copied to `locals[0..n)` by width; a `v128` uses the
  existing 16-byte `copyStackToLocal`. Floats use `loadFloatSlot` + `MOV_F32/64`
  because a float local is not an integer slot.
* Declared locals are zeroed on every loop iteration; otherwise a value written
  before the tail call leaks into the next invocation.
* The first attempt called `emitFuelCharge()` behind `internals.fuelMetering` —
  those are LibJIT names, not SLJIT ones. SLJIT has no fuel path, so the call was
  removed. (A future SLJIT fuel implementation must decide whether a tail call
  charges once per link.)
* The measure pass must resize the operand-stack model and mark the code
  unreachable in exactly the same way as the emit pass; the jump itself is
  emit-only.

### AsmJIT implementation notes

* AsmJIT had no body-start label. A new `asmjit::Label bodyStart` must be
  **created with `compiler.new_label()`** before binding it in the constructor;
  binding a default-constructed label fails at finalisation with error 12
  (`kInvalidLabel`). The first run failed exactly there.
* The emitter did not know its own module function index. The constructor now
  takes it (`importedFunctionCount + i`) so `dispatchReturnCall` can recognise
  `index == functionIndex`.
* Self arguments are copied into the parameter `locals` (vector buffers use
  `copyVector`), non-parameter locals are zeroed (`xor_`/`zeroVector`), then
  `jmp(bodyStart)`.
* `compile()` insists the function ends with its declared result arity. The loop
  never falls through, so `emitUnreachablePlaceholders()` fills the compile-time
  stack after the jump; the emitted placeholder instructions are dead.

### What was still open at the end of the first slice

**Mutual and indirect tail calls still used call + return.** (Superseded by item 1b below.) A cross-function
trampoline needs:

1. a driver per function (same C signature, exposed as `fnPtr`, raw body in
   `rawFnPtr`);
2. a uniform thunk per type (`tailEntry`) that loads arguments from
   `TailCallState::args`, calls the raw body, and stores the result;
3. a `dispatchReturnCall` path that writes `TailCallState` and returns instead
   of calling.

The core already defines `TailCallState`, and LibJIT implements all three. The
port can be JIT-generated (the LibJIT model) or a backend-neutral libffi closure
driver in WasmBase; the latter needs a `TailCallState::frame` field (or a
packed frame) before frame-form chains can be covered. Until that lands, the
handoff must not claim the constant-stack guarantee for mutual/indirect chains.

### Lessons

* **A frame-reusing branch must reinitialise everything the prologue did that
  matters to a fresh call** — parameters and declared locals; the stack guard is
  the one thing that must *not* run again.
* **A new control label is state, not an instruction.** It has to be created and
  bound before any branch can use it; AsmJIT's default label is not a valid id.
* **A backend's names are not the core's names.** `internals`, `fuelMetering`
  and `emitFuelCharge` are LibJIT concepts; copy the *contract*, not the
  identifiers.

## Item 1b — cross-function direct trampolines (new)

### Intended

Let a direct `return_call` to a *different* function run in constant native
stack on SLJIT and AsmJIT, not only a self-recursive one.

### Design that landed

`WasmBase/WasmTailTrampoline.{hpp,cpp}` is a backend-neutral table of **libffi
closures**:

* one **driver** per internal function, with the function's own C signature,
  exposed as `Callable::fnPtr`; the raw compiled entry moves to
  `Callable::rawFnPtr`;
* one **thunk** per function type, exposed as `Callable::tailEntry`, with the
  uniform `(ctx, fn, slots, results, buffer)` ABI;
* a driver calls the raw entry once, then loops while `TailCallState::pending`
  and the trap slot is clear, re-entering the target through its thunk until
  the chain ends. A single-result driver copies the final `results` slot into
  its return storage; a multi-result driver's buffer is already shared by the
  caller.

`Module::hasTailCalls()` is set by the validator, so a module without a tail
call keeps its raw entries and pays nothing. Installation is register-form
only (the frame ABI would need a frame pointer in `TailCallState`); a register
module's tail calls are marked at compile time with the same condition, and a
failed installation now **refuses the module** rather than emitting a pending
write whose driver does not exist.

The backends only learned to write `TailCallState` and return a typed zero. The
SLJIT direct-target address and the AsmJIT target materialisation are the only
backend-specific parts; both point at the instance's `Callable` vector, which is
sized before compilation and never moves afterwards.

### Evidence

`tailcall_mutual_probe.wat`, even/odd through direct `return_call`, 1,000,000
links:

```
TAILMUTUAL libjit: even0=1 run()=1
TAILMUTUAL asmjit: even0=1 run()=1
TAILMUTUAL sljit: even0=1 run()=1
```

`even0` is a direct `even(0)` call through a driver and guards the raw function
itself; `run` is the 1,000,000-link chain. The old call+return lowering hits the
guest stack guard on `run`.

### The five bugs this item exposed (in the order they appeared)

1. **Stale core ABI.** Adding a member to `Module` and a destructor/member to
   `ModuleInstanceInternals` changes their layout. LibJIT still used the old
   headers and crashed in `PreparedFunctionStack::prepare` before the first
   `TAILMUTUAL` line. Rebuilding every backend (not just the one under test)
   fixed it. **A core header change is a whole-tree rebuild.**
2. **The thunk read `args[2]` as the slot array.** libffi hands the handler an
   array of pointers to argument *values*; `args[2]` is the address of the slot
   pointer. It must be dereferenced like `args[3]`/`args[4]`. The first AsmJIT
   run segfaulted in the thunk for this reason.
3. **AsmJIT truncated the target pointer.** `mov(mem64, Imm(ptr))` is encoded as
   sign-extended imm32 when the pointer does not fit; the low 32 bits of
   `0x55ff9fd421a8` became `0xffffffff9fd421a8`. Materialising the pointer in a
   `Gp64` first fixes it. (SLJIT's `MOV reg, IMM` already handled 64 bits.)
4. **SLJIT's dummy return used an unmeasured stack slot.** The measure pass
   resized the model but the emit pass pushed a zero result and stored it;
   `stackSlotOffset` then pointed outside the allocated frame and SLJIT aborted
   in `sljit_emit_op1`. The dummy return is now emitted directly (void, integer,
   or float), with no operand-stack slot.
5. **SLJIT's `if/else` merge ignored the reachable arm.** When the else arm ended
   in a tail call (unreachable), `dispatchEnd` computed `endReachable` from the
   *current* (else) reachability only, so a reachable then arm was treated as
   dead and the raw function returned the dummy zero. `ControlFrame::thenReachable`
   records the then arm's end state; `endReachable` includes it. This was a
   **pre-existing** bug, not introduced by the trampoline; `even(0)` is its
   minimal reproducer.

### Indirect targets (same session)

`return_call_indirect` now takes the same pending path. The backend resolves the
table slot to a `Callable*` (the existing bounds/type/null checks are reused),
loads its `tailEntry`, and branches:

* non-null -> write `TailCallState` and return;
* null (an embedder-registered foreign funcref) -> the semantic call + return.

AsmJIT's resolver and invocation were split into `resolveIndirectTarget` and
`emitIndirectInvocation`, so the fallback calls the already-resolved target
instead of resolving the table twice. SLJIT saves the resolved callable in the
frame's scratch slot before calling `currentTailCallState`, because that helper
clobbers R0-R3; the fallback branch still finds the callable in R0.

Evidence (`tailcall_indirect_probe.wat`, even/odd through a two-entry funcref
table, 1,000,000 links):

```
TAILINDIRECT libjit: run()=1
TAILINDIRECT asmjit: run()=1
TAILINDIRECT sljit: run()=1
```

A first AsmJIT draft referenced a non-existent `typeIndexFor(calledType)` in the
new invocation helper; the declared type index is now passed explicitly. A
scripted edit also turned a `//! \return` in a `.cpp` comment into a carriage
return, splitting the line (`eturn ...`); a plain prose comment avoids the
escape.

### Frame-form targets (same session)

`TailCallState` now owns a 512-byte, 16-byte-aligned staging frame and the byte
offset of the last callee's result block. A frame driver is still
`(ctx, frame) -> void`; the thunk for a frame type lays the arguments out with
`frameArgumentOffset`/`frameValueBytes`, records `frameResultOffset`, and calls
the raw entry with `(ctx, TailCallState::frame)`. After the chain the driver
copies `resultBytes` from that result block back to the **original** caller's
frame at its own result offset. The backends' pending path in frame mode stores
the arguments in the uniform slots and returns `void`.

Evidence (`tailcall_frame_mutual_probe.wat`, four scalar parameters force the
frame ABI, 1,000,000 links):

```
TAILFRAME libjit: register even0=1 run()=1
TAILFRAME asmjit: even0=1 run()=1
TAILFRAME sljit: even0=1 run()=1
```

LibJIT keeps four scalar parameters in its register form, so that branch uses its
native trampoline; the frame driver is the SLJIT/AsmJIT path. The test asserts
both shapes.

### The eligibility bug this exposed

The first frame build made installation fatal when *any* type was outside the
model, but the emitters had already selected the pending path per callee. A frame
module with a `v128` (`tail_wide`) therefore refused installation while its body
still wrote `TailCallState`; the resulting mismatch crashed later tests and
`Runtime::unload`. `tailCallTrampolinesEligible(module, frameAbi)` is now a
**module-wide** decision computed by both the emitter and the installer, so a
module with one unsupported type keeps call + return for every tail call. This
was the direct lesson of the earlier "install failure must be fatal" fix: fatal
is only correct when eligibility is decided once, before emission.

### What is still open

### Function-reference targets (same session)

`call_ref` and `return_call_ref` are now lowered in the register form. AsmJIT's
`ref.func` representation changed from the function's **code address** to the
instance's **`Callable*`** (the representation SLJIT and LibJIT already used): a
reference call needs the context, dispatch and thunk fields, and pointer equality
is still function equality. `call_ref` checks the reference for null, selects the
native caller context when the callee has none, and invokes `fnPtr`; the tail form
loads `tailEntry` and takes the pending path, falling back to call + return for a
foreign reference without a thunk. Both backends refuse `call_ref` in frame form
rather than half-lower it.

Evidence (`tailcall_ref_probe.wat`, `ref.func` + `call_ref` entry, `return_call_ref`
links, 1,000,000 iterations):

```
TAILREF libjit: even0=1 run()=1
TAILREF asmjit: even0=1 run()=1
TAILREF sljit: even0=1 run()=1
```

One SLJIT bug: `dispatchCallRef` popped the reference from the operand-stack
model but did not resize the *argument* slots before pushing the result, so the
function returned the stale callable slot (a pointer). Resizing by the parameter
count before the result push fixed it. The reflection is a useful reminder that
the stack model is not the machine stack: every pop/push pair must be mirrored.

### Frame-form function references (same session)

Both backends already had a frame staging path for ordinary calls; AsmJIT's
`emitCallRefInvocation` now uses it for a `Callable` in a register: pack the
arguments into a 16-byte-aligned stack frame, invoke `(ctx, frame)`, read the
result block back, and let the existing trap/exception checks run. SLJIT already
routed a resolved callable through `emitResolvedCall`, which has both forms, so
its reference call and tail fallback needed no separate frame code.

Evidence (`tailcall_ref_frame_probe.wat`, four scalar parameters force the frame
ABI, 1,000,000 `return_call_ref` links):

```
TAILREFFRAME libjit: run()=1
TAILREFFRAME asmjit: run()=1
TAILREFFRAME sljit: run()=1
```

A second probe carries a scalar counter and **two v128 parameters** through the
same frame reference loop (two vectors force the frame ABI even where one would
fit SLJIT's register budget):

```
TAILV128REFFRAME asmjit: run()=1
TAILV128REFFRAME sljit: run()=1
```

LibJIT is excluded there because its native trampoline has never carried v128.

### Status

No tail-call form that either backend lowers is left. The matrix covers self,
direct, indirect and ref targets, scalar and v128 frames, and register and frame
ABIs.

## Item 2 — shared reference/table atomics

### Intended

Extend the numeric shared-everything atomics to reference slots (globals, struct
fields, array elements) and table slots, with the GC retain/release barrier.

### What we found, and fixed

This item started as a blocker and ended as an implementation. The blockers
were real, in this order:

1. **Shared reference shorthand was unparsed.** `(ref null (shared eq))` encodes
   as `0x63 0x65 0x6D` in explicit form, but a shared shorthand is `0x65 ht` at
   value-type position. The `0x65` byte is also the string-view `wtf16` value
   type, so `ValueType::decodeAfterFirstByte` now peeks one byte and only treats
   `0x65` as shared when it is followed by a known heap-type byte. Before this,
   the field decoder read the heap byte as the mutability flag and reported
   *malformed mutability*.
2. **The validator dropped sharedness and the heap type.** `sharedStorageValueType`
   built `ValueType{opcode, -1}`. It now goes through
   `Module::storageTypeFromCanon` (preserving heap type) and sets `isShared` for
   references, so a shared field operand matches its function parameter.
3. **Shared tables were unparsed for the same reason.** With the value-type fix
   and the table accessors below, the shared anyref/eqref table fixture loads and
   validates.
4. **The backends had no reference branches.** AsmJIT and SLJIT now route
   `set`/`xchg`/`cmpxchg` on a reference field/element through
   `wasm_atomic_ref_store`/`xchg`/`cmpxchg`; the numeric `wasm_atomic_*` family
   stays for scalar slots. SLJIT's five-argument reference cmpxchg is wrapped as
   `(ctx, address, pair, ordering)`, with the expected/replacement pair in the
   frame's scratch cell.
5. **Table atomics.** Core helpers `wasm_atomic_table_get/set/xchg/cmpxchg` do the
   bounds check and reference barrier; SLJIT packs the table index and ordering
   into one 32-bit selector where the four-argument ceiling requires it.

### Evidence

```
SHAREDREF libjit: run()=1 array=1
SHAREDREF asmjit: run()=1 array=1
SHAREDREF sljit: run()=1 array=1
TABLEATOMIC libjit: run()=1
TABLEATOMIC asmjit: run()=1
TABLEATOMIC sljit: run()=1
```

`shared_ref_atomic_probe.wat` covers a shared eqref struct field and array
(element) with set/xchg/cmpxchg/get; `table_atomic_probe.wat` covers shared
anyref/eqref tables with set/xchg/cmpxchg/get. Both include a null round-trip.

### Global reference atomics (same session)

The earlier "no valid fixture" conclusion was too narrow: the proposal accepts
an **unshared mutable `eqref`/`anyref`** global. `(global $g (mut eqref)
(ref.null eq))` plus set/xchg/cmpxchg/get validates, and all three backends run it
through the reference barrier.

```
GLOBALREF libjit: run()=1
GLOBALREF asmjit: run()=1
GLOBALREF sljit: run()=1
```

### v128 atomic slots are invalid, not a gap

The proposal is explicit: `global.atomic` allows i32/i64/anyref, and
`struct.atomic`/`array.atomic` allow i8/i16/i32/i64/anyref. The pinned tool
rejects v128 in all three. WasmJit's validator previously accepted the shape and
left a backend refusal to say "yet", which is the wrong layer and masks an
invalid module. `sharedStorageValueType` and the global atomic handlers now
reject f32/f64/v128 storage; three negative fixtures (global, struct, array) are
checked on all three backends:

```
V128ATOMIC libjit: rejected
V128ATOMIC asmjit: rejected
V128ATOMIC sljit: rejected
```

### What is still open

No reference/table atomic shape in the proposal surface is left. `v128` atomic
slots are not a missing feature; they are rejected by validation.

### Lessons

* **A parser desynchronization looks like a semantic error.** "malformed
  mutability" was a one-byte misread of a shared shorthand, not a mutability
  rule.
* **The type model is the gate.** Once `isShared` and the heap type survived
  canonicalization, the validator and both backends needed only the reference
  barrier branch.
* **A five-argument helper is a record, not a limitation.** The SLJIT cmpxchg
  pair in the scratch cell is the same technique the numeric selector already
  uses.

### Table accessors (same session)

Neither SLJIT nor AsmJIT lowered `table.get`/`table.set` at all, which was the
prerequisite for table atomics. Core helpers `wasm_table_get`/`wasm_table_set`
now do the bounds check and, for a write, the same `wasm_atomic_ref_store`
reference barrier as any other slot; the backends only pass `(ctx, table index,
element index)` and the value. `table.get` returns the opaque slot pointer and a
trap leaves it null; the backend's pending-trap check unwinds the function.

Evidence (`table_access_probe.wat`, externref and funcref tables, plus an
out-of-bounds get):

```
TABLEACCESS libjit: ext=1 fun=0
TABLEACCESS asmjit: ext=1 fun=0
TABLEACCESS sljit: ext=1 fun=0
```

`ext=1` is a null externref round-trip; `fun=0` is a `ref.func` staying non-null;
the out-of-bounds `table.get` throws at the host boundary. AsmJIT's first build
rejected the 32->64 index widening as a `mov`; `movzx` is the explicit form.

### Shared table atomics

The shared table fixture now loads and runs on all three backends; the four
atomic operations share the core bounds/barrier helpers. See the evidence block
above for `TABLEATOMIC`.

## Item 3 — AsmJIT block parameters

### Intended

Accept `block (param ...)`, `loop (param ...)` and `if (param ...)` in the
AsmJIT register-form emitter. Multi-value block *results* already worked; only
the parameters were refused.

### What we did

* `ControlFrame` now owns `paramTypes`, `params` and `paramCount`.
* `pushControlFrame` sets `stackHeight = values.size() - paramCount`, so the
  frame base is below the values the caller already pushed, and captures one
  register per parameter.
* `else` restores the parameter registers, not just the height; both arms start
  with the same values.
* `br`, `br_if` and `br_table` use the target's **parameters** for a loop and its
  **results** for a block, and transfer into the matching register vector.
* `try_table` arity accounts for a loop target's parameters (routing a non-empty
  payload to a loop is still refused in `emitCatchBranch`).
* `blockParamTypes` requires scalar parameters; `v128` parameters remain
  unsupported (and are now the durable unsupported probe in
  `asmjit_i32_unsupported.wat`).

### Evidence

`block_params_probe.wat` on all three backends:

```
BLOCKPARAM libjit: block=42 if=11/12 loop=0
BLOCKPARAM asmjit: block=42 if=11/12 loop=0
BLOCKPARAM sljit: block=42 if=11/12 loop=0
```

The first run failed for two **test** reasons, not backend reasons: the loop
fixture used `local.tee 1` without a declared local, and the `if` call passed the
condition as the first operand (the block parameter is *below* the condition).
Both were corrected; the backends agreed with each other before the test fix,
which is exactly why two backends agreeing is not a spec check.

### What is still open

`v128` block parameters; a catch payload routed to a loop with parameters.

### Lessons

* **`values.size()` is the top of the stack, not the base of the block.** The
  base is the top minus the parameters.
* **A branch target's signature is direction-dependent**: a loop expects its
  parameters, a block expects its results. Reusing `resultCount` for both is the
  bug the first `br` draft would have had.
* **Do not use a feature you intend to implement as the "unsupported" fixture**
  for a rollback test; move the probe to a genuinely excluded shape (`v128`
  parameter) when the feature lands.

## Remaining-gap pass — LibJIT frame calls, imports and multi-result

### Intended

Make the opted-in LibJIT frame form complete without changing the policy: the
register form stays the default and the frame form remains opt-in. Once selected,
a call, a native import, a multi-result signature and a frame tail must all work.

### Contract correction

The frame signature is now the canonical `void fn(VMContext*, void* frame)`.
The callee writes every result into the caller's result block; the driver no
longer receives a scalar return or writes it itself. Frame slots use the shared
`WASM::frameValueBytes` layout (8 bytes per scalar, 16 per v128), so the prologue
and result block agree with every other backend.

### What we did

* `LibJitTypeTranslator` spells both signature overloads as `(ctx, frame) -> void`.
* The prologue advances by `frameValueBytes`; the epilogue writes one or N results
  into the current frame (`storeFrameResults`) for implicit, explicit and trap exits.
* `emitFrameInvocation` allocates a local call frame with `jit_insn_alloca`, stages
  arguments at canonical offsets, invokes `fnPtr(ctx, frame)`, checks the pending
  exception slot, and reads the result block back.
* Direct, indirect and `call_ref` calls use that helper; native imports use the
  same `(ctx, frame)` contract.
* Frame tails are a frame call followed by a frame return. They are correct but
  not constant-stack; the register trampoline remains the constant-stack path.
* Frame thunks and the driver's trailing result-buffer parameter are disabled in
  frame mode, because the canonical signature has neither.

### Evidence

| Probe | Result |
|---|---|
| `LibJIT calls another function through an opted-in frame` | `via_call` result 6 at frame+48 |
| `LibJIT returns multiple results through an opted-in frame` | pair 7 / 9 |
| `LibJIT calls a native import through an opted-in frame` | host `(ctx, frame)` returns 42 |
| `LibJIT tail calls through an opted-in frame` | 100 links, result 1 |
| `LibJIT carries multi-result call_ref through an opted-in frame` | result 11 |
| `LibJIT returns a v128 through an opted-in frame` | lanes 7/8/9/10 |
| `LibJIT calls indirectly through an opted-in frame` | table call result 10 |
| `LibJIT rejected module leaves the runtime usable` | runtime survives a rejected artifact |

The register-form tests stay green and the opt-in is still default-off.

### Mistakes caught

* The two call lowering bodies (`dispatchCall` and `dispatchCallThroughCallable`)
  share their stack-pop text; the first edit landed each frame block in the other
  function. Scoped replacement fixed it.
* The driver still tried to read the trailing result-buffer parameter in frame
  mode, where the signature has only `(ctx, frame)`; that was the multi-result
  `call_ref` crash.
* Frame-mode tail thunks were still built against the two-parameter frame signature;
  frame mode now has no tail thunks.
* The `FuncType` overload of `translateFunctionSignature` still returned a scalar;
  the `CoreTypeView` overload had been changed first.

### Remaining limitation

Frame tails are call+return, so they grow the host stack. Constant-stack frame tails
in LibJIT would need a frame-aware trampoline; that is separate from the call,
import and multi-result contract this pass closes.

## Remaining-gap pass — v128 block results

### Intended

A structured block, loop or `if` may produce a `v128` result, not only a scalar.
This was the last result-shape refusal on SLJIT and AsmJIT.

### What we did

* AsmJIT allocates a vector buffer for a v128 block result and `storeBlockResult`
  uses the vector-aware `transferValue` (16-byte copy). Both the shorthand block
  type and a type-index block signature accept v128.
* SLJIT's shorthand block-type switch now maps v128 to a 16-byte result slot; the
  type-index path already accepted it. Branch transfers were already 16-byte
  aware from the block-parameter work.

### Evidence

`block_result_v128_probe.wat` (fallthrough, explicit `br`, both `if` arms, loop):

```
BLOCKRESV128 libjit: block=5 branch=6 if=7/8 loop=9
BLOCKRESV128 asmjit: block=5 branch=6 if=7/8 loop=9
BLOCKRESV128 sljit: block=5 branch=6 if=7/8 loop=9
```

The rollback fixture moved again: `asmjit_i32_unsupported` is fully supported,
so the durable AsmJIT refusal is now `br_table` to a multi-result function label
(`asmjit_brtable_multi_unsupported.wat`). The SLJIT refusal test became a
positive `v128` block-result test.

## Remaining-gap pass — catch payload to a parameterised loop

### Intended

A `try_table` catch may target a loop whose parameters receive the caught
payload. AsmJIT refused every non-empty payload routed to a loop.

### What we did

`emitCatchBranch` no longer has a separate zero-payload-only loop path. It
checks the carried arity against `paramCount` for a loop and `resultCount` for a
block, clears the pending exception first (a zero-payload catch is still a
catch), loads each payload field and writes it into the loop parameter (scalar
register, or two qwords into the v128 buffer), writes the `exnref` when the
clause carries one, and jumps to `begin` for a loop or `end` for a block.

### Evidence

`catch_loop_param_probe.wat`: loop parameter 0 enters the loop; the `try_table`
throws 42; the catch restarts the loop with 42 and the loop exits with it.

```
CATCHLOOP libjit: run()=42
CATCHLOOP asmjit: run()=42
CATCHLOOP sljit: run()=42
```

## Remaining-gap pass — frame-form indirect tail calls

### Intended

A `return_call_indirect` in a frame-form module must take the driver/thunk path,
not call + return.

### What we did

* SLJIT removed the `!frameAbi_` guard: `tailCallFrameSupported` selects the
  pending path, and `emitTailCallIndirectState` emits a void dummy return in
  frame mode. The existing `emitResolvedCall` frame fallback is reused.
* AsmJIT's `emitIndirectInvocation` gained the frame staging branch (pack the
  argument block, invoke `(ctx, frame)`, read every result). The frame
  `return_call_indirect` branch now resolves the callable, checks `tailEntry`,
  writes `TailCallState` and uses the frame invocation only as the foreign-target
  fallback.

### Evidence

`tailcall_indirect_frame_probe.wat`, four parameters, even/odd through a funcref
table, 1,000,000 links:

```
TAILINDIRECTFRAME libjit: run()=1
TAILINDIRECTFRAME asmjit: run()=1
TAILINDIRECTFRAME sljit: run()=1
```

## Remaining-gap pass — multi-result reference calls

### Intended

Let `call_ref` and `return_call_ref` carry the full result tuple, not just one
value. Multi-result modules use the frame ABI, so the result block is the carrier.

### What we did

* AsmJIT's frame `emitCallRefInvocation` reads **every** result from the frame
  result block (scalar register or vector buffer) and pushes it; the frame
  multi-result return path writes every value at `frameResultOffset`.
* SLJIT's `emitResolvedCall` frame branch copies every result to the operand-stack
  slots starting at `base`; `dispatchCallRef` pushes all result widths; the tail
  fallback uses `emitReturnValue`'s multi-result frame path.
* SLJIT's ABI selection now chooses the frame form when a function has more than
  one result, not only when its argument slots exceed four.

### Evidence

`tailcall_ref_multi_probe.wat`: `$tail` forwards to `$pair` with
`return_call_ref`, both returning `(i32, i32)`; `run` adds them.

```
TAILREFMULTI asmjit: run()=11
TAILREFMULTI sljit: run()=11
```

### Mistakes caught

* The first AsmJIT edit hit the **ordinary frame call** block (same text), not
  `emitCallRefInvocation`; the `call_ref` path still read one result. Fixing the
  right block made the probe pass.
* SLJIT's two-result module previously kept the register ABI and was refused at
  the function prologue; the ABI selector now includes `results.size() > 1`.
* `sljit: refuses a module whose function it cannot compile` used a two-result
  fixture as its unsupported probe; it became a positive v128 block-result test
  when that shape landed.

## Remaining-gap pass — v128 block parameters (SLJIT, AsmJIT)

### Intended

Lift the last structured-control parameter restriction: a block/loop/if may carry
a `v128` parameter. The value is a 16-byte cell, not a scalar.

### What we did

* AsmJIT's `blockParamTypes` now accepts `v128`; a loop back-edge uses
  `transferValue`, which copies 16 bytes with `copyVector` instead of moving a
  pointer.
* SLJIT's block signature accepts a 16-byte parameter and `transferToBase` copies
  the two 8-byte halves; the single-word `loadSlot`/`storeSlot` cannot transfer a
  vector.

### The AsmJIT control-flow bug this exposed

An `if (result i32)` whose **else** arm ended in `br $loop` left the branch
operands in the compile-time value stack. `dispatchEnd` then called
`storeBlockResult` on the unreachable else arm and refused the module
(*"structured block leaves an unexpected operand stack shape"*). AsmJIT had no
reachability tracking at all.

`IntegerFunctionEmitter` now tracks an `armTerminated` flag:

* `br`, `br_table`, `return`, every `return_call` form and `unreachable` clear it;
* a control frame records `entryReachable` and, for an `if`, `thenReachable`;
* `else` stores the then result only when the then arm fell through;
* `end` stores the current arm only when it is live; a block counts as reachable
  when it was entered live, a branch targets its end, or the then arm reached it;
* an unconditional branch to a block marks `endTargeted`, so its transferred
  result survives an otherwise dead arm.

This is the same class of defect as the earlier SLJIT `thenReachable` fix; AsmJIT
had simply never needed the flag while its block results were scalar-only.

### Evidence

`block_params_v128_probe.wat`:

```
BLOCKV128 libjit: block=42 if=7/8 loop=7
BLOCKV128 asmjit: block=42 if=7/8 loop=7
BLOCKV128 sljit: block=42 if=7/8 loop=7
```

The rollback fixture (`asmjit_i32_unsupported.wat`) used v128 block *parameters*
as its deliberate failure, then a v128 block **result**; both are supported now,
so it moved to `br_table` to a multi-result function label.

### What is still open

`v128` block **results** remain outside the scalar result subset; the parameter
form above is complete.

## Item 4 — bulk memory on memory64 (SLJIT, AsmJIT)

### Intended

Lower `memory.copy`, `memory.fill` and `memory.init` when the memory uses the
64-bit index type, instead of refusing the function (SLJIT) or the module
(AsmJIT).

### What we did

* Added `ModuleInstance::memoryInit64`; the existing 32-bit `memoryInit`
  delegates to it. The bounds checks use subtraction after a `>` test so a huge
  64-bit offset cannot wrap.
* Added backend helper entry points with 64-bit addresses/lengths:
  `asmjitMemoryCopy64`/`Fill64`/`Init64` and
  `sljitMemoryCopy64`/`Fill64`/`Init64`.
* AsmJIT: the invoke signature now matches the wasm operand widths — a 64-bit
  destination, but a **32-bit** segment source/length for `memory.init`. The
  first attempt declared all three as 64-bit and the upper halves of the 32-bit
  operands were garbage, which trapped `memory.init` as out of bounds.
* SLJIT: the four-argument call ceiling cannot carry `(vm, memoryPair, dst, src,
  len)`. `memory.fill64` fits as `(vm, memoryAndValue, dst, len)`. `copy64` and
  `init64` take a pointer to the **first operand slot** and read the three slots
  (16 bytes apart) inside the helper. The first attempt tried to materialise
  `SLJIT_SP` in a register with `MOV R3, SP`, which trips SLJIT's argument
  assertion (`SLJIT_SP` is a base, not an op1 source); `slotAddress(R3, base)`
  (`sljit_get_local_base`) is the supported way.

### Evidence

`memory64_bulk_probe.wat` on all three backends:

```
MEM64BULK libjit: copy=97 fill=65 init=97
MEM64BULK asmjit: copy=97 fill=65 init=97
MEM64BULK sljit: copy=97 fill=65 init=97
```

The probe exercises a 64-bit destination, a 64-bit source/length and a load
back from the copied/filled/initialised bytes. The old `memory64_bulk` fixture's
refusal tests became execution tests; the module's zero-length copy is legal.

### A semantic trap in the fixture

The first probe used one **active** data segment and called `memory.init` on it.
All three backends correctly trapped: an active segment is dropped after
instantiation. A `memory.init` test needs a **passive** segment. This was not a
backend bug; the debug print showed `segsize=7` but `segmentSize=0` because the
dropped flag had been set.

### Lessons

* **Match the helper signature to the wasm operand width, not to the widest
  one.** A 32-bit segment index passed through a 64-bit parameter carries
  whatever was in the upper half of the register.
* **SLJIT cannot read SP as an ordinary register.** Use `sljit_get_local_base`
  (the existing `slotAddress`) when a helper needs a stack address.
* **A bulk-memory test must use a passive segment for `memory.init`.** Active
  segments are dropped at instantiation, exactly as the spec says.

## Remaining-gap pass — SLJIT legacy-EH rethrow frame sizing

### Intended

Make the `[legacy-eh]` group finish under `WASMJIT_BACKEND=sljit`. The legacy
exception rewrite is frontend desugaring (`docs/proposals/LEGACY_EXCEPTIONS.md`
`LE-3`/`LE-4`); the remaining failure was in the SLJIT lowering of the rewritten
final-form `try_table`.

### Symptom

The first eight cases passed, then the rethrow case aborted the process:

```
Assertion failed at extern/sljit/sljit_src/sljitLir.c:1708
#4 sljit_emit_op1
#5 Sljit::OpcodeDispatcher::dispatchI32Const(int)
```

That assertion is `FUNCTION_CHECK_SRC` rejecting a `SLJIT_MEM1(SLJIT_SP)`
offset greater than or equal to `compiler->logical_local_size`. The function
frame was one operand slot too small.

### Root cause

`try_table` catches are **edges**, not inline arms. `emitCatchBranch` writes the
payload into the target block's base and jumps to that block's end label, and
sets `target.endTargeted = true`. The measuring pass skips
`emitExceptionPropagation` entirely (it emits no code), so it never performed
that bookkeeping. `dispatchEnd` therefore treated the catch target as
unreachable, did not push its result widths, and the catch arm that follows was
measured as dead code.

In the emitting pass the same target *was* marked by `emitCatchBranch`; the arm
became reachable and its `i32.const` pushed one slot past the measured frame.
The depth-measuring and emitting passes had silently diverged, which is exactly
what the two-pass design is supposed to prevent.

### Fix

* `dispatchTryTable` now marks every non-loop catch target `endTargeted` in the
  measuring pass, mirroring `emitCatchBranch` in the emitting pass. The target's
  payload slots and the reachable catch arm are then included in the
  high-water mark.
* `push` now refuses a push past the measured frame in the emitting pass, and
  records the function index in `unsupportedReason_`. A future pass divergence
  becomes a clean per-function compile refusal instead of an SLJIT assertion
  that aborts the host (`docs/SAFETY.md`).

### Evidence

```
WASMJIT_BACKEND=sljit ./Test/WasmJit '[legacy-eh]' --rng-seed 12345
All tests passed (110 assertions in 12 test cases)
```

This covers same-function rethrow, cross-function rethrow, a void catch-all
rethrow and the cleanup/exit path.

### Lessons

* **A measuring pass must model every edge the emitting pass creates.** A catch
  clause has control-flow consequences even though it has no inline body.
* **“Unreachable” in the linear walk is not “unreachable” in the program.** A
  handler target can be reachable only through an exception edge.
* **Convert backend assertions into refusals at the first frame boundary.** The
  new `push` guard is a last line of defence; the measured capacity remains the
  primary contract.

## Remaining-gap pass — SLJIT SIMD shuffle and v128 results

### Intended

Finish the `simd: shuffle and swizzle run through the JIT` case and the
`v128` calling-convention cases under `WASMJIT_BACKEND=sljit`. The full generic
run stopped here with a SIGSEGV inside generated code.

### Symptom

`simd_shuffle`'s first export crashed while writing its 16-byte result. The
faulting instruction was `mov %rax,(%rdi)` with `%rdi` equal to the generated
code page address. The callee had used its **second incoming argument** as the
result-buffer pointer; the caller had never passed one.

The next case, `simd: a v128 parameter and result round-trip through the JIT`,
then crashed with `%rdi = 0x0807060504030201`, the first two lanes of the input
vector. The test had called a frame-ABI module with `WASM::callCallable<V128>`,
which passes the vector as the second C argument; the callee correctly treated
that second argument as a frame pointer.

### Root cause

Two separate caller-side omissions:

1. SLJIT's register-form call path handled a scalar result but not the canonical
   `v128` result shape: `callArgTypes` declared the function as returning a
   word, and the call supplied no trailing out-buffer. The callee's prologue
   (`resultViaBuffer`) had always expected `void(VMContext*, params..., void* out)`.
   The register file therefore contained the callable target where the callee
   expected the buffer, and `emitReturnValue` wrote to executable memory.
2. `ModuleCompiler` correctly switches a whole module to the staging-frame ABI
   when any signature needs more than four argument slots. The `simd_abi`
   fixture has a four-`v128` function, so all of its exports are frame-form. The
   test's direct `callCallable` calls bypassed the frame ABI.

### Fix

* `callArgTypes` now derives parameter slots with `WASM::mapArgSlots` (a `v128`
  parameter consumes two), declares a `v128` result as `void`, and adds the
  trailing out-buffer pointer at the first slot after all parameters.
* The register-form `emitResolvedCall` uses the 16-slot staging area after the
  operand stack as that out-buffer, then copies both words into the one
  16-byte operand slot. The staging area is reserved for both call forms now.
* The `simd_abi` test marshals through `callCallableFrame` whenever
  `instance.frameCallAbi()` is set, including v128, i32 and i64 results.

### Evidence

```
WASMJIT_BACKEND=sljit ./Test/WasmJit 'simd: shuffle and swizzle run through the JIT'
All tests passed (25 assertions in 1 test case)

WASMJIT_BACKEND=sljit ./Test/WasmJit 'simd: a v128 parameter and result round-trip through the JIT'
All tests passed (31 assertions in 1 test case)

WASMJIT_BACKEND=sljit ./Test/WasmJit 'spec: simd_splat'
All tests passed (2 assertions in 1 test case)
```

The full SLJIT run now reaches the component-script cases instead of stopping
at SIMD shuffle.

### Lessons

* **A `v128` result is an extra hidden argument, not a wide return register.**
  The caller and callee must agree on that trailing pointer or the callee writes
  through whatever occupied the argument slot.
* **Frame mode is module-wide, so tests must ask the instance which ABI it
  got.** A direct native call is only valid for the register form.
* **Use `mapArgSlots` on both sides.** It is exactly the helper whose contract
  prevents the two-slot vector shift from being recomputed by hand.

### What remains

The full SLJIT run still stops later, in `component script marshals structured
values`; that is the next item, not a SIMD issue.

## AsmJIT — branch to the function label in frame mode

### Intended

Make `spec: func` instantiate under `WASMJIT_BACKEND=asmjit`. It failed before
any command could run:

```
instantiation of 'func.0.wasm' failed:
AsmJit compiler finalisation failed: 25
```

### Diagnosis

`func.0` contains a 17-parameter / 16-result `large-sig`, so `moduleNeedsFrame`
selects the staging-frame ABI for the whole module. Function 75 is:

```wat
(func (result i32)
  i32.const 79
  br 0)
```

Its `br 0` targets the function label. `dispatchBr` and `dispatchBrIf` treated
that target as a register-form return: they called `emitFunctionReturn`, which
emits `ret(value)`. In frame mode the generated signature is
`void(ctx, frame)`, so the register assignment for a non-void return was
invalid. AsmJIT reports this at `finalize()` as error 25
(`InvalidAssignment`), long after the body was emitted.

The diagnosis used a temporary `ErrorHandler` plus a per-function compile limit
to bisect the 97-function module; the handler and probes were removed after the
fix.

### Fix

Extract the return sequence into `emitFunctionReturnValues()`, which already
knows both ABI shapes (`dispatchReturn` used it for the frame path). `dispatchBr`
and the conditional function branch in `dispatchBrIf` now call it. A branch to
the function label therefore writes the result block and emits `ret` in frame
mode, and keeps the register return in register mode. `dispatchBrTable` already
went through `dispatchReturn`.

### Evidence

```
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: func'
[spec] func: passed=100 failed=0 skipped=75
All tests passed (2 assertions in 1 test case)
```

### Lessons

* **A branch to the outermost label is a return, and must use the same ABI as
  `return`.** The three control paths had drifted: only `dispatchReturn`
  honoured the frame form.
* **Frame mode is a module-wide property.** One large signature changes the
  meaning of every function's `ret`, including tiny functions with no frame
  parameters of their own.
* **Bisect with a compile limit, not a body skip, when the fault may be in
  prologue/signature handling.** The temporary `ASMJIT_LIMIT_FUNCTION` probe
  found function 75; a body-only skip would have hidden it behind the same
  empty-function path.

### Follow-up — tail-call test marshalling

The same frame-mode module property affected `tail calls compute correct
results`: the multi-result `pair_via_tail` signature makes AsmJIT compile the
whole `tail_call` module in frame form. The test still called the exports with
`WASM::callCallable`, so `fac_acc` received its first `i64` argument as the
frame pointer and crashed in the first call.

The test now checks `instance.frameCallAbi()` and builds the canonical frame for
every call, with register-form calls retained for LibJIT. One detail mattered:
frame results occupy an 8-byte slot even for `i32`, while the public tuple is
packed; `pair_via_tail` must compact the second slot from byte 16 to byte 4.
The frame bytes observed were `07 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00`
at the result block, which is the backend doing the right thing.

Evidence:

```
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'tail calls compute correct results'
All tests passed (21 assertions in 1 test case)
```

**Lesson:** an ABI marshalling test has two layouts to respect — the native call
shape and the public tuple layout. A raw `memcpy` from a frame result block is
not the latter when scalars are slot-padded.

## Component runtime — frame-form core modules

### Intended

Let the component-script and component-runtime structured-value cases run under
the SLJIT and AsmJIT backends instead of segfaulting in generated code.

### Root cause

A component core module commonly exports a canonical `realloc` with four
parameters (`(i32, i32, i32, i32) -> i32`). With the implicit `VMContext*` that
is five argument slots, so both backends correctly compile the **whole module**
in the staging-frame form. The component runtime still invoked core functions
through the register-form `WASM::callCallable` path in two places:

* `invokeCore`, which performs canonical lift/lower calls; and
* `allocate`, which invokes the module's `realloc`.

The first argument (a pointer-sized value) was therefore interpreted as the
frame pointer, and the callee dereferenced it.

### Fix

* Added `callableUsesFrameAbi`: a compiled core callable (`rawFnPtr` non-null)
  carries a `VMContext*` whose address is the `ModuleInstance`, so the runtime
  can ask that instance for its module-wide ABI. Native imports and closures
  stay on the register form.
* `invokeCore` now marshals the canonical frame directly: 8-byte slots for the
  core parameters, a `void(VMContext*, void*)` call, and 8-byte result slots
  read back into `WASM::Value`.
* `allocate` pads the four `realloc` arguments into a frame and reads the pointer
  result from the following slot.
* The two component tests that call `runtime.coreFunction(0)` directly now use
  `callCallableFrame` when the core instance reports frame mode.

### Evidence

```
WASMJIT_BACKEND=sljit ./Test/WasmJit 'component script marshals structured values'
All tests passed (2 assertions in 1 test case)

WASMJIT_BACKEND=sljit ./Test/WasmJit 'component runtime lowers and lifts a map'
All tests passed (10 assertions in 1 test case)
```

The full SLJIT run then advances through the former string-test crash as well;
both backend-selected runs now complete all 821 cases without a process signal
(SLJIT 702/119, AsmJIT 649/172). The remaining failures are functional gaps, not
frame-ABI marshalling.

### Lessons

* **Module-wide frame mode reaches every consumer, not just direct Wasm calls.**
  The canonical ABI runtime is a caller too.
* **A callable's context is the instance for compiled functions.** `rawFnPtr`
  distinguishes that case from a native import whose context is host state.
* **Test the ABI at the boundary where the component runtime actually crosses
  it.** A passing register-form direct call can hide a frame-form-only module.

## SLJIT — i64 bit counts and calls in dead code

### Intended

Get `spec: br_table` past its first module. The run failed at instantiation:

```
the SLJIT backend could not compile function 2:
the SLJIT backend does not lower dispatchI64Ctz
```

### Root cause

Two independent gaps:

* SLJIT had no `i64.clz` / `i64.ctz` / `i64.popcnt` overrides; only the i32
  family existed.
* The `br_table` script's `as-block-first` shape contains a call in polymorphic
  dead code after a `br`. `emitCall` still asserted `reachable_` instead of
  no-opping, even though the measuring/emitting walk already treats such code
  as unreachable.

### Fix

* Added the three i64 bit-count overrides. `i64.popcnt` calls the existing
  `sljitI64Popcnt` helper through `SLJIT_ARGS1(W, W)`; `clz`/`ctz` use
  `emitIntegerUnary` with an 8-byte result width.
* Added the same `if (!reachable_) return;` guard to direct/indirect/reference
  calls and to all four tail-call dispatchers. Dead calls are part of the
  polymorphic stack, not a backend error.

### Evidence

```
WASMJIT_BACKEND=sljit ./Test/WasmJit 'spec: br_table'
[spec] br_table: passed=162 failed=0 skipped=24
All tests passed (2 assertions in 1 test case)
```

### Lessons

* **Complete the width families together.** `i32` and `i64` share the lowering
  helpers; a missing 64-bit sibling only shows up when a fixture reaches it.
* **Reachability is an instruction property, not a call-only exception.** Every
  lowering that asserts a live operand stack must first respect dead code.

## SLJIT — five-slot SIMD load-lane signatures

### Intended

Make `spec: simd_load32_lane` instantiate. It failed at function 0 with:

```
the SLJIT backend cannot pass 5 argument slots:
the target has only 4, so this signature needs fewer vector parameters
```

### Root cause

The module-wide frame decision counted only declared Wasm parameter words. The
function `(i32, v128) -> v128` has three parameter words, so it stayed in the
register form. The register form also passes a hidden trailing out-buffer
pointer for a `v128` result, making five C argument entries, one above
`kRegisterArgumentCeiling`.

The same audit found two smaller problems in the decision:

* it was duplicated between the dispatcher and the module compiler, with the
  emitter's ceiling spelled as a local constant;
* an earlier attempt to include imported signatures switched whole host-import
  modules to the frame form, which crashed an AssemblyScript native-interop
  test. A host import is a register-form native C function; it must not choose
  the caller's module ABI.

### Fix

* Moved the ceiling to `kRegisterArgumentCeiling` in
  `SljitOpcodeDispatcher.hpp` and made the emitter use it.
* Added `signatureNeedsFrame()` to the module compiler. It counts
  `VMContext*`, every parameter word, and the hidden `v128` result buffer, and
  treats a multi-result signature as frame-form.
* Kept the decision scoped to defined functions. The comment now states why
  imported signatures are excluded, so the next attempt does not rediscover the
  crash.

### Evidence

```
WASMJIT_BACKEND=sljit ./Test/WasmJit 'spec: simd_load32_lane'
[spec] simd_load32_lane: passed=21 failed=0 skipped=3

simd_load8_lane  49/0     simd_load16_lane 33/0
simd_load64_lane 13/0
```

### Lessons

* **Count the ABI the code generator emits, not the Wasm signature.** A hidden
  result buffer is a real argument entry.
* **A module-wide ABI decision must name whose ABI it chooses.** Defined
  functions and host imports do not share one.
* **The same constant must live in one place.** The dispatcher and the mode
  decision drifting apart is how the off-by-one returned.

## Both backends — deterministic NaN canonicalisation

### Intended

Make `the deterministic profile canonicalises observable NaNs` green on SLJIT
and AsmJIT. The test covers `i32.reinterpret_f32`, `i64.reinterpret_f64`,
`f32.store`, `f64.store`, `global.set`, a single float return and a two-result
float return.

### Root cause

The deterministic profile's `N` marker was plumbed into LibJIT only. SLJIT and
AsmJIT never canonicalised at any observability point.

Implementing it exposed a second, independent defect: the `ceil`/`floor`
helpers forwarded a signalling NaN to libm. glibc preserves the signalling
pattern, while the spec's result class is an *arithmetic* NaN, so `spec: f32`
and `spec: f64` reported `nan:arithmetic` failures even though every other
float operation was green.

### Fix

* Added bit-pattern canonicalisation to both backends
  (`sljitCanonicalF32Bits`/`sljitCanonicalF64Bits`,
  `asmjitCanonicalF32Bits`/`asmjitCanonicalF64Bits`). The bits are classified
  directly, so non-NaN values including `-0` stay exact and a signalling NaN is
  not quieted by the classification itself.
* Passed the profile into each per-function emitter. Canonicalisation is a
  no-op unless `ProfileConfig::deterministic()` is set.
* Applied it at the host-observable boundaries: float-to-integer `reinterpret`,
  `f32.store`/`f64.store`, `global.set`, single and multi-result returns, and
  AsmJIT's function-label branches.
* Made `ceil`/`floor` return `quiet_NaN()` for NaN input in both backends.
* Fixed SLJIT helper-call clobbering. The first implementation computed the
  memory address or the frame pointer in `R0`/`R2`, then called the
  canonicalisation helper, which clobbers every scratch register, and stored
  through the now-clobbered pointer. The fix is ordering: canonicalise before
  computing the address, and canonicalise all multi-results before loading the
  frame pointer. This was a real SIGSEGV, caught by the profile test.
* Used a bit-level helper per backend instead of funnelling the value through an
  `FR`/Xmm register; a float move may quiet or re-sign a signalling NaN before
  it can be classified.

### Evidence

```
WASMJIT_BACKEND=sljit  ./Test/WasmJit 'the deterministic profile canonicalises observable NaNs'
All tests passed (23 assertions in 1 test case)

WASMJIT_BACKEND=asmjit ./Test/WasmJit 'the deterministic profile canonicalises observable NaNs'
All tests passed (23 assertions in 1 test case)

WASMJIT_BACKEND=sljit ./Test/WasmJit 'spec: f32'   -> 2501 passed / 0 failed
WASMJIT_BACKEND=sljit ./Test/WasmJit 'spec: f64'   -> 2501 passed / 0 failed
```

### Lessons

* **A helper call is a scratch-register barrier.** Any pointer or frame address
  that must survive it belongs in a frame slot or must be recomputed after it.
  This is the second time the campaign hit this class in SLJIT.
* **Do the policy transform before the address calculation.** The reverse order
  looks natural and is wrong.
* **“The hardware returns a NaN” is not enough for the spec.** The result class
  is a property of the opcode, not of libm.
* **One profile test found two defects.** It caught the missing policy and the
  pointer-clobber crash; keep it backend-selected.

## AsmJIT — i64 bit counts and dead-code tolerance

### Intended

Add the missing i64 `clz`/`ctz`/`popcnt` lowerings, then advance
`spec: br_table`.

### Root cause

* AsmJIT had `i32` bit counts only; `dispatchI64Clz`/`Ctz`/`Popcnt` fell through
  to the base refusal.
* `br_table.0` contains calls, drops, selects, stores, conversions and
  arithmetic in polymorphic dead code after a terminator. AsmJIT's emitter
  treated `armTerminated` as advisory in only a few places, so the next
  value-consuming opcode threw an operand-stack underflow instead of emitting
  nothing.
* `br_table` required every target to have an identical stack height and an
  identical type vector. Wasm only requires the same arity: targets can sit at
  different nesting depths, and a `funcref` target can accept a
  `(ref null $t)` value.
* `br_if` to a block result with the branch value missing is dead code, not an
  arity error.
* `dispatchBrTable` cleared the compile-time value stack after emitting its
  cases. A nested target block's `end` then resized the cleared vector and
  produced default-constructed (invalid) registers for the instruction that
  followed the branch.
* `table.get` widened an i32 index with `movzx r64, r32`. That is not an x86
  encoding; the program was rejected only at finalisation with error 53
  (`OperandSizeMismatch`).

### Fix

* Added `emitBitCount64` and `emitUnaryCall64`, plus the three i64 overrides.
* Added dead-code guards to value-consuming lowerings. They are arity-aware
  (`values.size() < required`) rather than unconditional, so a catch-entry path
  that is linearly “terminated” but has values still lowers.
* `br_table` validation now checks arity only; each target's own result types
  drive its transfer. The operand-stack check is an underflow check, not an
  equality check.
* `br_if`/`br`/`br_table` mismatches return instead of throwing when the arm is
  dead.
* Introduced `inDeadCode()` = `armTerminated && module.tags.empty()`. AsmJIT
  does not yet model exception catch edges as reachability, so EH modules opt
  out of dead-code skipping. Without this, the first version silently dropped
  catch arms and regressed eight legacy-EH tests. The residual EH dead-code
  failures are pre-existing and loud, not wrong results.
* `dispatchIf` treats an `if` with no values as dead, but lowers an `if` with a
  condition even when `armTerminated` is set, because that is a catch entry.
* A `br_table` case targeting the function label restores both the value stack
  and `armTerminated` after `dispatchReturn`; otherwise the first case's return
  suppressed every later case's return (this produced `51` instead of `50` in
  `func.wast`).
* Void functions that end in dead code clear the synthesized placeholder stack
  before emitting the void return.
* Stopped clearing `values` after `br_table`; a target block's `end` now trims
  it back to that block's own stack height, preserving the base operands.
* Replaced the table index `movzx` with `mov dst.r32(), src.r32()` (the x86-64
  way to zero-extend an i32 into a 64-bit register) in `table.get`, `table.set`
  and the table atomic forms.
* Used AsmJIT's `kValidateIntermediate` diagnostic option temporarily to make
  the invalid instruction fail at its emit site instead of at `finalize()`.
  With that evidence, the two fixes above took the script to green; the option
  is not left enabled in production.

### Evidence

```
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: func'
[spec] func: passed=100 failed=0 skipped=75

WASMJIT_BACKEND=asmjit ./Test/WasmJit 'the opt-in rewrite runs the superseded exception form'
All tests passed (13 assertions in 1 test case)

WASMJIT_BACKEND=asmjit ./Test/WasmJit 'a delegate that skips an intervening catch reaches its target'
All tests passed (10 assertions in 1 test case)
```

`spec: br_table` is green: **139 passed / 0 failed / 47 skipped**. The
`values.clear()` fix and the index-widening fix are both required; either one
alone leaves a different failure.

Full runs after the batch (serial, same seed):

```
SLJIT   821 cases: 720 passed / 101 failed
AsmJIT  821 cases: 712 passed / 109 failed
```

### Lessons

* **`armTerminated` is not a complete reachability model.** It is the linear
  arm state; catch edges are a second entry. A guard that assumes otherwise
  silently deletes live code.
* **Match the spec's arity rule, not an accidental equality.** `br_table`
  targets share arity, not identity.
* **Restore every piece of emitter state a helper mutates.** `dispatchReturn`
  changes both the value stack and the arm flag.
* **A late finaliser error can still be an emitter typo.** Error 53 was an
  invalid `movzx`, not register allocation. Enable intermediate validation
  while diagnosing, then remove it.
* **A vector-sized compile-time stack needs vector-sized cleanup.** Clearing it
  after a multi-target branch loses the base operands every nested block end
  expects.

## Shared — table.grow, v128 globals and integer SIMD lanes

### Intended

Close the `table_size`/`table_grow` gap and the `v128` global/lane gaps that
were blocking several SIMD scripts and the `simd_globals` fixture.

### Root cause

* `table.grow` had no core helper and neither backend overrode
  `dispatchTableGrow`. `StoreOwnedTable::grow` exists, but calling it directly
  bypasses the Store's allocation budget and maximum-size policy.
* AsmJIT's test session accepted only i32 parameters, so table probes with a
  reference argument were skipped; `table.grow` looked broken when the
  invocation had never happened.
* SLJIT's global get/set copied only one word from a global cell; AsmJIT
  rejected `v128` globals in `validateGlobalIndex`.
* AsmJIT had only `i32x4` extract/replace lane; the other integer lane widths
  fell through to the base refusal.

### Fix

* Added `wasm_table_grow` to `WasmTable.{hpp,cpp}`. It validates the table,
  recovers the `ModuleInstance` from the `VMContext` (the two are the same
  address by contract), calls `ModuleInstance::growTable` so the Store owns the
  allocation policy, fills the new slots through the reference barrier, and
  returns the previous size or all-ones on failure. A failed grow is not a
  trap.
* Wired `dispatchTableGrow` into SLJIT and AsmJIT. The delta is zero-extended
  from i32 with a 32-bit `mov` into the low half of a fresh 64-bit register.
* Extended the AsmJIT test session with the fixed table shapes it actually
  needs: `(i32)->ref`, `(i32,ref)->void`, `(i32)->i32`, `(i32,ref)->i32`. This
  is deliberate test-harness coverage, not a general libffi path.
* Implemented `v128` global get/set as two 8-byte copies in both backends.
  SLJIT uses the 16-byte operand slot directly; AsmJIT materialises a 16-byte
  stack buffer, matching its existing v128 representation.
* Added AsmJIT `i8x16`/`i16x8`/`i64x2` extract and replace lane helpers, plus
  the float lane paths already present. The shared core helpers do the lane
  arithmetic; the backend only marshals the 16-byte buffer.
* Updated the `simd_globals` test to marshal a `v128` parameter through the
  frame ABI when the instance selects it. A module with a `v128` parameter is
  frame-form in AsmJIT; the generic `callCallable` helper is register-form.

### Evidence

```
WASMJIT_BACKEND=sljit  ./Test/WasmJit 'spec: table_grow'  -> 51 passed / 0 failed
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: table_grow'  -> 39 passed / 0 failed
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: table_size'  -> 37 passed / 0 failed
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: table_set'   -> 14 passed / 0 failed
WASMJIT_BACKEND=asmjit ./Test/WasmJit 'simd: v128 globals run through the JIT'
All tests passed (13 assertions in 1 test case)

WASMJIT_BACKEND=asmjit ./Test/WasmJit 'spec: table_get' -> 4 passed / 0 failed
```

The last `table_get` failure was the harness, not the table read path: the
script's `init` function has one `externref` parameter, which the AsmJIT test
session still rejected. Adding the `(ref)->void` shape made the real table
initialisation run and the whole script pass.

### Lessons

* **Use the Store path for anything that allocates guest memory.** A direct
  `StoreOwnedTable::grow` skips the budget and leaves the instance's view
  inconsistent.
* **A skipped test is not a passing test.** The AsmJIT size-after-grow failures
  were the harness refusing to invoke a reference parameter.
* **A 32→64 zero extension is a 32-bit `mov`, not `movzx`.** The same mistake
  appeared in table index widening and was the hidden finalisation failure.
* **Keep the backend representation at the boundary.** v128 globals are two
  words in a cell; each backend maps them to its own buffer/slot form.

## Shared — SIMD splat/lane, register v128 invokes, empty modules

### Intended

Finish the remaining SIMD splat and lane scripts, then remove the last
harness-level `v128` refusal on SLJIT.

### Root cause

* AsmJIT had only `i32x4.splat`; `i8x16`, `i16x8` and `i64x2` fell through to
  the base refusal. `simd_lane` and all of `simd_splat` were blocked on that.
* SLJIT's register-form test session refused every `v128` parameter/result:
  libffi was only given scalar types. Once it accepted v128, a second defect
  appeared in the prologue: the hidden result-buffer pointer was read from
  `S(nextSlot - 2)`, a count that includes float parameters. For
  `(f32) -> v128`, that reads an uninitialised S2 instead of the pointer in S1.
* An AsmJIT module with no functions still created a `CodeHolder`, finalised it
  and called `JitRuntime::add`, which rejects zero-sized code with
  `NoCodeGenerated` (error 10). Empty declaration-only modules therefore failed
  to instantiate and took several spec scripts with them.

### Fix

* AsmJIT: one integer-splat emitter shared by `i8x16`, `i16x8` and `i64x2`,
  using the existing `wasm_*_splat` helpers.
* SLJIT session: a `v128` parameter is two `ffi_type_uint64` arguments (the two
  halves of the 16-byte slot); a `v128` result appends a pointer to the caller's
  16-byte result buffer.
* SLJIT prologue: count *integer* argument entries separately from argument
  ordinals. The result buffer is `S(integerEntries)`, so a preceding float does
  not shift it. `savedCount` stays the declared-argument count that
  `sljit_emit_enter` validates against; the extra saved register for a
  float+pointer shape is harmless.
* AsmJIT: return early from `compileFunctions` when there are no bodies, after
  clearing the function records. No executable allocation is attempted.

### Evidence

```
SLJIT  spec: simd_splat       162 passed / 0 failed
SLJIT  simd_store8/16/32/64_lane  49/33/21/13 passed, 0 failed
AsmJIT spec: simd_splat       162 passed / 0 failed
AsmJIT spec: simd_lane        286 passed / 0 failed
AsmJIT spec: simd_const       517 passed / 0 failed
```

Full serial runs:

```
SLJIT   821 cases: 720 passed / 101 failed
AsmJIT  821 cases: 712 passed / 109 failed
```

The AsmJIT jump from 679 to 712 includes the empty-module fix and the SIMD
splat/lane work. One component tag case (`tags.wast` `run-a`) is newly exposed
now that those modules instantiate; it is a functional failure, not a signal.

### Lessons

* **An empty module is still a legal module.** Do not allocate or install code
  just because a compiler object exists.
* **Float and integer argument registers are independent sequences.** Any
  saved-register index derived from a mixed ordinal is a bug waiting for the
  first float-before-pointer signature.
* **A harness refusal can hide several real successes.** The SLJIT v128 result
  buffer path had never run for a float-parameter function until libffi could
  describe it.
* **One missing splat blocks an entire proposal script.** Complete the width
  family, as with the bit-count family earlier.

## Shared — block/if branches and multi-result frames

### Intended

Close the remaining AsmJIT branch family (`br`, `br_if`, `loop`, `labels`, `if`)
and make the multi-value JIT-to-JIT fixture exercise the frame ABI without
crashing.

### Root cause

* `br` and `br_if` used the same exact `values.size() == target stack height +
  arity` check that `br_table` had. Wasm permits extra values below the branch
  operands; only an underflow is invalid. This rejected valid `br`/`br_if`
  modules.
* An `if` without `else` was rejected whenever it had results. Wasm allows it
  when the parameters have the result shape; the false path forwards them. The
  true path can still compute a different value, so the two paths need separate
  result handling.
* Multi-result calls were still refused in frame mode on both backends even
  though the frame form carries a result block, and the compile-time stack only
  pushed the first result.
* `call_indirect` classified an out-of-range index as an ordinary OOB table
  access. The spec distinguishes `undefined element` (index out of range) from
  `uninitialized element` (null slot).
* The multi-value host test used the register `callCallable` helpers on a module
  that had switched to the frame ABI, and read frame result slots as packed
  tuple fields.

### Fix

* `br`/`br_if`: accept extra values for block targets; keep the strict shape for
  **loop** back-edges, where the next iteration re-enters with only its declared
  parameters. The strict loop check is deliberate: relaxing it produced an
  AsmJIT infinite loop in `loop.0`.
* `if` without `else`: lower the true arm, store its result, jump over the
  alternate, bind the alternate with parameter-to-result forwarding, then bind
  the end. An empty true arm forwards the parameters through the same path.
* Frame multi-result calls: allow more than one result when `frameMode`, and
  push all declared result widths onto the compile-time stack.
* `call_indirect`: out-of-range index -> `UndefinedElement`; null slot ->
  `UninitializedElement`.
* Test: marshal the multi-value module through `callCallableFrame` when the
  instance selects the frame ABI, and compact 8-byte frame result slots into the
  public packed tuple (`i32` at offsets 0/4).

### Evidence

```
AsmJIT  spec: br          77/0
AsmJIT  spec: br_if       84/0
AsmJIT  spec: labels      26/0
AsmJIT  spec: if         125/0
SLJIT   spec: if         125/0
Both    multi-value: JIT-to-JIT calls marshal every result  16 assertions green
```

Full serial runs:

```
SLJIT   821 cases: 724 passed / 97 failed
AsmJIT  821 cases: 721 passed / 100 failed
```

AsmJIT `spec: loop` still fails (a remaining loop-target stack-shape issue), but
it now fails deterministically rather than timing out.

### Lessons

* **Branch arity is not branch stack height.** The spec allows discarded values
  for block targets but a loop back-edge must model exactly the next iteration's
  parameters.
* **An implicit else is a real control path.** It must forward parameters, while
  the true arm may store a different result.
* **Frame result slots are not the public tuple layout.** A 4-byte `i32` result
  occupies an 8-byte frame slot; embedders compact explicitly.
* **Trap kinds are part of the API contract.** `undefined element` and
  `uninitialized element` are distinct cases, not one generic OOB trap.

## SLJIT — frame-mode large signatures and measurement widths

### Intended

Make `spec: call` and `spec: stack` instantiate. Both failed at the first
function with a large frame signature.

### Root cause

* In frame mode, the prologue still counted every float parameter and requested
  that many scratch FR registers. `call.wast`'s
  `return-from-long-argument-list-helper` has a 100-parameter signature with
  dozens of floats; the target has far fewer FR scratch registers, so
  `sljit_emit_enter` aborted the process.
* The frame-form call staging area was fixed at 16 slots (256 bytes), smaller
  than that signature's argument block.
* The measuring pass of `emitEqz` (and `emitCompare`) returned before changing
  the operand-stack width to the i32 result width. A following `if` then saw the
  old i64 width and rejected the condition as "not an i32".

### Fix

* Frame mode requests only the three fixed scratch FR registers; parameter
  floats live in the caller's frame and need no FR register.
* Added `kStagingCallSlots = 128` (2 KiB) for frame-form calls, shared by the
  frame size and the call-path guard. The exact byte count is still checked
  before emitting.
* `emitEqz` and `emitCompare` set the stack width to 4 before the measuring-pass
  return. This is the same measure/emit divergence class that produced earlier
  frame bugs; the result kind is a compile-time property and belongs in both
  passes.

### Evidence

```
SLJIT spec: call    73 passed / 0 failed
SLJIT spec: stack    7 passed / 0 failed
SLJIT full serial   821 cases: 729 passed / 92 failed
AsmJIT full serial  821 cases: 721 passed / 100 failed
```

### Lessons

* **Frame mode changes the register budget, not just the call shape.** Do not
  reserve one register per value that already lives in memory.
* **A fixed scratch area is a hidden signature limit.** Size it from the
  largest signature the engine accepts, and keep the exact check at the call
  site.
* **The measuring pass must model result types even when it emits nothing.**
  Width is part of the compile-time stack, not an emission side effect.

## Both backends — imported index spaces, reachability, and tail calls

### Intended

Close the shared spec clusters that were still blocked at instantiation:
`global`, `memory`, `load1`, `store1`, `store2`, `fac`, `loop`, `call_ref`,
`return_call`, `return_call_indirect`, `return_call_ref`, and the AsmJIT
frame-mode import refusal.

### Root cause

* **Imported index spaces.** `Module::globals` and `Module::memories` contain
  only *defined* entries; imports live in `importGlobals` / `importMemories`.
  Both backends looked an instruction's index up in the defined-only vector, so
  `global.get 17` or `i32.load` on an imported memory was rejected as out of
  range even though the runtime's context uses imports-first indexing.
* **Measuring pass width.** SLJIT's `emitFloatCompare` returned from its
  measuring pass without changing the result width from the float operands'
  width to i32, so a following `if` rejected the condition.
* **Zero-arity branch reachability.** AsmJIT marked a block's `end` reachable
  only when a `br_if` transferred a value. A `br_if` to an empty block therefore
  left the block end marked dead and every following instruction was skipped as
  unreachable; `loop.wast`'s `nesting` lost its divide and decrement.
* **Frame staging size.** AsmJIT allocated `argBytes + 16` for every frame call,
  but a callee can write more than two 8-byte result slots. `pick1` wrote three
  results into a 32-byte block and overwrote the caller's spilled `VMContext`;
  the resulting crash depended on stack layout, which is why it disappeared
  under a debugger.
* **Tail-call fallback stack.** The non-thunk path emitted `emitResolvedCall`
  followed by `emitReturnValue`, but `emitResolvedCall` does not update the
  compile-time operand stack. The return saw an empty stack.
* **Tail-call arity.** A tail call discards everything below its arguments; only
  underflow is invalid. AsmJIT required exact equality.
* **Null function references.** SLJIT loaded the trap kind into `R1` while its
  helper ABI takes the first argument in `R0`, so it reported kind 0
  (`unreachable`). AsmJIT reported generic `NullReference`.
* **Imports and the module ABI.** AsmJIT refused every frame-mode module with an
  import, and let an imported signature force the whole module into frame mode.
  Neither is necessary: a native import is a typed-register entry, a Wasm import
  uses its exporting instance's form, and only defined functions choose the
  module's own ABI.

### Fix

* Both backends resolve global and memory indices imports-first, then
  definitions; the AsmJIT frame-mode native-import path invokes the typed
  register ABI while the guest keeps its staging frame.
* SLJIT `emitFloatCompare` sets the i32 result width in the measuring pass.
* AsmJIT marks a block end targeted for every non-loop branch form, including
  zero-arity `br_if`; the loop back-edge shape check is underflow-only, because
  extra operands are discarded by the branch.
* `frameCallStorageBytes` sizes every direct, indirect and `call_ref` staging
  block from the actual parameter plus result types.
* Both tail-call fallbacks push the callee's result widths before
  `emitReturnValue`; all three tail-call forms accept extra operands below their
  arguments.
* `NullFunctionReference` is used for `call_ref` / `return_call_ref` null
  references, with the kind in `R0` on SLJIT and a dedicated AsmJIT helper.
* AsmJIT's module ABI decision ignores import signatures; the updated
  `asmjit_import_call` test checks that a frame-mode module can call a native
  import instead of expecting a refusal.

### Evidence

```
SLJIT  spec: global 77/0     spec: memory 65/0    spec: load1 18/0
SLJIT  spec: store1 13/0     spec: store2 25/0    spec: fac 8/0
SLJIT  spec: loop 79/0       spec: call_ref 31/0  spec: return_call 37/0
SLJIT  spec: return_call_indirect 53/0           spec: return_call_ref 40/0
AsmJIT spec: global 51/0     spec: memory 40/0    spec: load1 18/0
AsmJIT spec: store1 5/0      spec: store2 25/0    spec: fac 8/0
AsmJIT spec: loop 79/0       spec: call_ref 10/0  spec: return_call 37/0
AsmJIT spec: return_call_indirect 53/0           spec: return_call_ref 12/0
```

Full serial runs:

```
SLJIT   821 cases: 751 passed / 70 failed; no signal
AsmJIT  821 cases: 743 passed / 78 failed; no signal
```

### Lessons

* **An index space is not a vector.** Imports precede definitions in every
  runtime index space; a backend that indexes the defined-only vector is wrong
  even when the runtime context it emits against is correct.
* **Frame blocks need the whole result block, not a guessed 16 bytes.** Reserve
  from the callee's actual result slots, or a third result silently corrupts the
  caller's frame.
* **Zero-arity branches still make control flow reachable.** Reachability is a
  property of the target, not of how many values are transferred.
* **A call helper and a return helper do not share a stack model.** If the call
  path does not push results, the return path must be given them explicitly.
* **A tail call is a branch, not an exact-arity call.** Extra operands below the
  arguments are discarded by both.
* **An import does not define the importer's ABI.** The call site must bridge
  the callee's form; only defined functions choose the module-wide form.
* **Trap-kind ABI bugs are invisible until the trap is asserted.** The same
  wrong register produced a plausible but wrong kind (`unreachable`), so only
  the kind-sensitive spec assertion caught it.
* **`Callable` is an aggregate without initialisers for all fields.** A test
  that sets only `fnPtr` leaves `context` unspecified; a nonzero residue makes
  a native import look like a cross-module Wasm import. Value-initialise the
  descriptor (`Callable host{}`) or set every ABI field.


## Both backends — relaxed SIMD, bitmask, i31 and extern conversions

### Intended

Close the relaxed-SIMD set, `simd_boolean`, `ref_eq` and the i31 construction
needed by the GC reference scripts.

### Root cause

* Every relaxed opcode (`relaxed_swizzle`, `relaxed_trunc`, `relaxed_madd`/
  `nmadd`, `relaxed_laneselect`, `relaxed_min`/`max`, `relaxed_q15mulr_s`, the
  two relaxed dots) was still the base dispatcher's `onUnhandledOpcode`.
* `i8x16/i16x8/i32x4/i64x2.bitmask` were missing, although the shared reduction
  helpers already existed.
* `ref.i31` and `i31.get_s/u` were missing. The runtime represents an i31 as a
  tagged immediate (`(zero-extended value << 1) | 1`), so no GC header exists to
  read.
* `any.convert_extern` / `extern.convert_any` were missing. The core already
  implements the bridge as `ModuleInstance::externBox` / `externUnbox`.

### Fix

* Relaxed SIMD lowers through the existing unary, binary and (new) ternary v128
  helper paths on both backends. The ternary helper has the same
  `(a, b, c, out)` shape as `v128.bitselect`; SLJIT now shares that path instead
  of repeating it.
* Bitmask ops use the existing `wasm_<lane>_bitmask` reduction helpers.
* `ref.i31` shifts the zero-extended operand left by one and sets the tag.
  `i31.get_s` uses a 32-bit arithmetic shift and `i31.get_u` a 32-bit logical
  shift, with a null check that reports `NullI31Reference`.
* The extern bridge calls `externBox`/`externUnbox` through non-throwing C
  wrappers, so an allocation failure reports a trap instead of escaping.
* A first attempt to fold i31 into `ref.cast`/`ref.test` was backed out: it
  misclassified a non-i31 table value under ASLR and dereferenced it. The i31
  construction stays; `ref.cast` i31/None remains a named gap.

### Evidence

```
Both   spec: i32x4_relaxed_trunc 1/0      spec: i8x16_relaxed_swizzle 6/0
Both   spec: relaxed_dot_product 11/0     spec: relaxed_madd_nmadd 19/0
Both   spec: relaxed_min_max 25/0         spec: relaxed_laneselect 12/0
Both   spec: i16x8_relaxed_q15mulr_s 3/0  spec: simd_boolean 261/0
Both   spec: ref_eq 83/0
```

Full serial runs:

```
SLJIT   821 cases: 763 passed / 58 failed; no signal
AsmJIT  821 cases: 753 passed / 68 failed; no signal
```

### Lessons

* **A shared helper is the whole lowering.** Relaxed SIMD is a large opcode set
  but a small implementation when the strict/relaxed rule already lives in the
  backend-neutral helpers.
* **Tagged immediates must be checked before header loads.** i31 is the case
  that turns a cast into a wild pointer dereference; the safe fix is to branch
  on the tag first, and back out a partial cast extension until that is done.
* **A trap helper must not throw.** The extern bridge allocates, so its C
  wrapper catches and converts to a trap; otherwise a host allocation failure
  would terminate the process.

## Both backends — GC array construction/init and packed structs

### Intended

Close the GC array scripts and `struct` on both backends.

### Root cause

* `array.new_elem`, `array.init_elem` and `array.init_data` were unhandled.
  The core only had whole-array copy helpers; no range-checked sub-range form
  existed for `array.init_*`.
* Expression-form element segments were rejected outside `funcref` arrays. The
  spec uses `i31ref` (and later typed GC refs) segments whose entries are cached
  in `elementSegmentRefs` at instantiation.
* `array.new_data` allocated before checking the source segment, so an absurd
  length reported out-of-memory instead of out-of-bounds memory access (GC-5).
* AsmJIT `array.set` scaled `i8`/`i16` elements by 8 bytes, so packed writes
  landed past the element.
* AsmJIT `struct.set` rejected packed fields; SLJIT `struct.set` and default
  zero-fill rejected them, and SLJIT `struct.set` reported `NullReference`
  instead of `NullStructureReference`.

### Fix

* Added `ModuleInstance::arrayInitDataRange` / `arrayInitElemRange` with
  destination-array and source-segment checks, and routed both backends through
  their C wrappers.
* Added `ModuleInstance::arrayNewData` / `arrayNewElem`: validate the source
  range first, then allocate, then copy. This also fixes GC-5 trap ordering.
* Element copies now use the cached evaluated segment references and retain GC
  references (`refClone`); index-form `funcref` segments keep their direct
  callable mapping.
* Added a ternary helper and full array dispatch on AsmJIT; SLJIT uses a
  five-word descriptor in its call staging area because its helper ABI accepts
  at most four C arguments.
* Fixed AsmJIT `array.set` scaling and packed `struct.set`; added SLJIT packed
  struct zero-fill/set and corrected its null-structure trap kind.
* SLJIT deliberately refuses `array.new_elem` with typed GC-reference elements;
  that shape produced an ASLR-sensitive wild pointer. i31/funcref segments are
  supported, and the refusal is a clean compile-time diagnostic.

### Evidence

```
AsmJIT array                 31/0    AsmJIT array_new_elem 8/0
Both   array_init_elem       33/0    Both array_init_data 44/0
SLJIT  array                 36/12 (module with typed GC refs refused)
SLJIT  array_new_elem        22/2  (same typed-GC-ref refusal)
SLJIT  struct                25/0    AsmJIT struct 18/0
```

Full serial runs:

```
SLJIT   821 cases: 766 passed / 55 failed; no signal
AsmJIT  821 cases: 758 passed / 63 failed; no signal
```

### Lessons

* **Range semantics belong in the core.** Packing five arguments through a
  four-argument ABI is possible, but the checked operation itself should be one
  helper, not an allocation plus a copy the caller must order correctly.
* **Trap precedence is observable.** “Check the source before allocating” is a
  spec rule, not an optimisation.
* **Expression-form element segments are references, not indices.** Reuse the
  cached evaluated reference and retain it; re-evaluating or treating it as a
  function index is wrong.
* **Packed element scaling is log2(bytes), not a two-case guess.** `i8` and
  `i16` need shifts 0 and 1, not 3.
* **A refusal is better than a wild pointer.** The typed-GC-ref `array.new_elem`
  shape stays explicitly unimplemented on SLJIT until its ownership/layout is

## Both backends — GC reference casts and branch-on-cast

### Intended

Close `ref_cast`, `br_on_null`, `br_on_non_null`, `br_on_cast` and
`br_on_cast_fail`, plus AsmJIT `ref_as_non_null`.

### Root cause

* `br_on_null` rejected any target with label values; it must transfer the
  values **below** the reference and drop the reference.
* `br_on_non_null` was unhandled, and its fallthrough consumes the reference
  (the branch carries it only on the non-null path).
* `br_on_cast` / `br_on_cast_fail` were unhandled.
* `ref.cast`/`ref.test` read the object header directly, which is invalid for a
  tagged i31 immediate and for the `none` bottom type.
* A first i31 patch in `ref.cast` caused an ASLR-sensitive wild dereference and
  was backed out; the final implementation avoids header reads entirely.
* Non-nullable `ref.cast` on null is a **cast failure**, not a null-structure or
  null-i31 trap; `ref.as_non_null` is the operation that reports
  `NullReference`.

### Fix

* Use the backend-neutral `ModuleInstance::refMatchesHeapType` predicate for
  `ref.test` and `ref.cast` on both backends. It handles i31, none, extern and
  object references without a header dereference.
* Added `sljitRefMatchesHeapType` / `asmjitRefMatchesHeapType` wrappers taking a
  packed `(isTypeIndex, heapType)` value and the nullable flag.
* Generalised SLJIT’s `transferToBase` with an end-depth so `br_on_null` can
  transfer the range below the reference.
* Implemented `br_on_non_null`, `br_on_cast` and `br_on_cast_fail`, including
  function-label branches (conditional function return) and loop targets.
* SLJIT now reports `CastFailure` for a non-nullable `ref.cast` of null;
  AsmJIT `ref.as_non_null` reports `NullReference`.

### Evidence

```
Both ref_cast              45/0
Both br_on_null             9/0
Both br_on_non_null        11/0
Both br_on_cast            31/0
Both br_on_cast_fail       31/0
Both ref_as_non_null        6/0
```

Full serial runs:

```
SLJIT   821 cases: 771 passed / 50 failed; no signal
AsmJIT  821 cases: 764 passed / 57 failed; no signal
```

### Lessons

* **Do not classify a tagged immediate by reading a header.** A single predicate
  in the core (`refMatchesHeapType`) is both simpler and safe for i31/none.
* **`br_on_null` and `br_on_non_null` have opposite fallthrough stack effects.**
  One leaves the reference, the other consumes it.
* **A branch to a function label is still a branch.** It needs the same
  conditional predicate and fallthrough label as a block target.
* **Null handling is per-instruction, not per-type.** `ref.cast` on null is a
  cast failure; `ref.as_non_null` on null is `null reference`.


## SLJIT switch/call_indirect64 and AsmJIT unreachable valid code

### Intended

Close SLJIT `switch` and `call_indirect64`, and make AsmJIT accept valid
polymorphic dead code (`unreached-valid`).

### Root cause

* SLJIT `i32.wrap_i64` returned from the measuring pass without changing the
  result width, so a following `br_table` saw an i64 selector.
* SLJIT `call_indirect` required an i32 selector even on a 64-bit table. The
  table64 flag was already known but not used for the selector width.
* AsmJIT ref operations asserted on an empty value stack in unreachable code.
  Valid polymorphic code may contain such instructions after `unreachable`.

### Fix

* `i32.wrap_i64` sets the i32 width in both passes.
* Both `call_indirect` and `return_call_indirect` derive the selector width from
  the table's 64-bit flag.
* Added `inDeadCode()` guards to AsmJIT `ref.is_null`, `ref.as_non_null`,
  `ref.eq`, `ref.cast`/`ref.test` and the extern conversion helpers.
* A throw-terminator experiment was reverted: setting `armTerminated` after
  `throw` made `throw_ref` catch clauses disappear (linear catch arms were
  skipped). `throw_ref` stays green; `throw`'s dead polymorphic second throw
  remains the known gap.

### Evidence

```
SLJIT switch            27/0
SLJIT call_indirect64    2/0
AsmJIT unreached-valid  12/0
AsmJIT throw_ref        13/0
```

Full serial runs:

```
SLJIT   821 cases: 773 passed / 48 failed; no signal
AsmJIT  821 cases: 765 passed / 56 failed; no signal
```

### Lessons

* **Measure-pass widths keep recurring.** `i32.wrap_i64` is the same class as
  `eqz`/`compare`; every width-changing opcode needs both passes.
* **A table's index width is a property of the table, not the instruction.**
  `call_indirect` and `return_call_indirect` must read the table64 flag.
* **EH-aware dead-code tests are not the same as terminator state.** A throw
  inside a try_table must leave the linear catch arms reachable; marking the
  arm globally terminated hides them.


## SLJIT — try_table catches that target the function label

### Intended

Close the last SLJIT `try_table` case.

### Root cause

The dispatcher required every catch label to name an enclosing structured
frame. A `catch`/`catch_ref` may also target the function label, where the
payload becomes the function's return values.

### Fix

* Validation now accepts `clause.labelIdx == tryFrameIndex` as the function
  label and checks its arity against the function result widths.
* Added `emitCatchReturn`: it clears the pending exception, materialises the
  payload (and trailing exnref for `catch_ref`) at the base of the operand
  stack, and emits the function return.
* The measuring pass reserves the function-result slots for that catch arm.

### Evidence

```
SLJIT try_table 56/0
SLJIT full      821 cases: 774 passed / 47 failed; no signal
```

SLJIT’s remaining spec failures are the typed-GC-reference `array.new_elem`
shape and `linking`/`linking1`.

### Lesson

A catch target is a branch label, and the function label is one of them. It
needs the same arity check and a return path rather than a structured-frame
jump.

## Linking — an imported callable runs in the exporting instance

### Intended

Close `linking` and `linking1` on both backends, which failed identically.

### Root cause

The imported callable was resolved correctly: `importStorage[i].context` pointed
at the exporting instance, and the backends' Wasm-to-Wasm call path already
selected it. The runner then defeated that work: `ScriptRunner::invoke` and
`Diagnose` always passed the *invoking module's* `instance.context()` to
`backend.invoke`.

For an ordinary export those are the same context. For a re-export of an
imported function they are not: `Nm`'s exported `Mm.load` is `Mm`'s code, so it
must read `Mm`'s memories and tables. Passing `Nm`'s context made it read
`Nm`'s memory instead — exactly the observed `0xf2` instead of `2`.

### Fix

* `ScriptRunner::invoke` and `Diagnose::invokeExport` use
  `callable.context != nullptr ? callable.context : instance.context()`.
* No core or backend change was needed: the import context had been carried
  correctly all along.

### Evidence

```
SLJIT  linking  112/0   linking0 5/0   linking1 14/0   linking2 11/0   linking3 13/0
AsmJIT linking  112/0   linking0 5/0   linking1 14/0   linking2 11/0   linking3 13/0
```

Full serial runs:

```
SLJIT   821 cases: 776 passed / 45 failed; no signal
AsmJIT  821 cases: 767 passed / 54 failed; no signal
```

### Lessons

* **A `Callable`'s context is part of the callable.** An invocation entry point
  must not substitute the caller's context; only a native import, whose context
  is null, falls back to the caller.
* **When two backends fail one script identically, look above the backend
  seam.** The linking failures were a runner bug, not two code-generator bugs.


## AsmJIT — measuring the register ABI, and `table.fill`'s call-site order

### Intended

Stop the AsmJIT spec run from mistaking a test-harness limitation for a backend
capability. `AsmJITSession::invoke` enumerated C signatures by hand (all-i32, or
a few reference shapes), so every other signature was reported as “could not
marshal the arguments” and skipped.

### Root cause and fix

* `TestBackend` now calls the register form through libffi, as the SLJIT session
  already did. The whole scalar/ref signature space the backend emits is
  measurable; only `v128` remains outside this path.
* That exposed a real AsmJIT bug in `table.fill`: the table-index virtual
  register was created and defined **after** the invoke node. An argument
  defined after the call site is not live at the call, so the helper received a
  stale register (`5` instead of `0`) and filled the wrong table. The register is
  now materialised before the invoke node, matching `emitMemoryAddress`'s rule.

### Evidence

```
AsmJIT atomic        249/0
AsmJIT memory_trap   182/0
AsmJIT memory_grow64  49/0
AsmJIT table_fill     36/0
```

Full serial runs:

```
SLJIT   821 cases: 776 passed / 45 failed; no signal
AsmJIT  821 cases: 774 passed / 47 failed; no signal
```

All AsmJIT spec scripts are green after the EH fixes below; SLJIT's remaining
spec failures are the typed-GC-reference `array.new_elem` and `array` cases.

### Lessons

* **A skip is not a pass.** The hand-written signature table hid `atomic`,
  `memory_trap`, `memory_grow64` and `table.fill` behind “could not marshal”.
  Using libffi made the measurement about the backend again.
* **Materialise every call argument before its invoke node.** This is the same
  rule that made memory addressing reliable; `table.fill` had one late
  definition left.


## AsmJIT — terminator-aware `throw` and catch edges that reach a label

### Intended

Close AsmJIT's last exception scripts: `throw`, `throw_ref` and `try_table`.

### Root causes

* **A second `throw` is polymorphic.** `inDeadCode()` intentionally reports
  false whenever a module has tags, so the dead second `throw` in
  `throw-polymorphic` still tried to pop a payload. The arm's own terminator
  state is the correct guard. `dispatchThrow` now returns through
  `emitUnreachablePlaceholders()` when `armTerminated` is set, and marks the arm
  terminated after emitting the first throw.
* **A catch edge can be the only path to a block end.** `emitCatchBranch` jumped
  to `target.end` without setting `target.endTargeted`. When the try body always
  throws, the continuation after that block was emitted as dead code, leaving
  `catch-complex-1` with an uninitialised result.
* **A catch may target the function label.** AsmJIT required every catch label
  to be strictly outer than the `try_table`. `labelIdx == frameIndex` is the
  function label; it is now validated against the function result count and
  lowered by `emitCatchReturn`, which clears the pending exception, loads the
  payload (plus `exnref` for `catch_ref`) and returns.

### Evidence

```
AsmJIT throw       10/0
AsmJIT throw_ref   13/0
AsmJIT try_table   56/0
```

Full serial runs:

```
SLJIT   821 cases: 776 passed / 45 failed; no signal
AsmJIT  821 cases: 774 passed / 47 failed; no signal
```

Every AsmJIT spec script is now green.

### Lessons

* **Dead-code predicates are not interchangeable.** `inDeadCode()` is EH-aware
  and deliberately false in a tagged module; a throw's own arm state is not the
  same question.
* **A catch is a branch, including its reachability effects.** Jumping to a
  label without marking it targeted hides the code at that label.
* **The function label is a valid catch target.** It needs the same arity check
  and a return lowering, exactly as SLJIT's `emitCatchReturn` already did.


## SLJIT — typed-GC-reference `array.new_elem`

### Intended

Close SLJIT's last two spec-failure cases, `array` and `array_new_elem`.

### Root cause

`array.new_elem` itself was already correct: the core copies the cached
expression-form segment references and retains each GC object. The SLJIT refusal
had masked a separate bug in `array.set`, one instruction later in the failing
`set_get` fixture.

`dispatchArraySet` loaded the value slot with a hard-coded 4-byte width. For an
i64 element or a GC reference that truncates the stored 8-byte word to its low
32 bits. The first `array.new_elem` checks still passed because they only read;
`$set_get` stored the truncated pointer, then `array.get_u` dereferenced it and
the child died with SIGSEGV.

### Fix

* Removed the SLJIT refusal for typed-GC-reference elements.
* `dispatchArraySet` loads the value at its declared width
  (`packed ? 4 : width`): packed values still arrive in a 4-byte i32 slot, while
  references and i64 values keep all 8 bytes.

### Evidence

```
SLJIT array             48/0
SLJIT array_new_elem    24/0
SLJIT array_init_elem   33/0
```

Full serial run:

```
SLJIT   821 cases: 778 passed / 43 failed; no signal
```

Every SLJIT spec script is now green. Both backends' remaining failures are
non-spec suites.

### Lessons

* **A refusal can hide the real defect at the next instruction.** Removing it
  exposed a width truncation, which was the actual cause of the old
  ASLR-sensitive wild pointer.
* **Slot width is per-value, not per-operator family.** `array.set` cannot load
  every value as 4 bytes any more than `array.get` can store every result as 8.


## Common — table64 results and the wide-arithmetic family

### Intended

Close two shared feature gaps: `memory64` table size/grow and
`i64.add128` / `i64.sub128` / `i64.mul_wide_s` / `i64.mul_wide_u`.

### Root causes

* **Table size/grow always used the 32-bit shape.** Both backends loaded the
  delta as 4 bytes, returned a 32-bit value and (for `table.size`) truncated the
  helper's result. On a table64, `table.grow` failure (`UINT64_MAX`) therefore
  reached the caller as `1`, and a large size could be truncated.
  The index width is now derived from the table's `0x04` limits flag; the
  helpers return `uint64_t`, and only an i32 table narrows the result slot.
* **The wide-arithmetic opcodes had no lowering.** Add/sub are emitted inline
  with carry/borrow (`adc`/`sbb` on AsmJIT; a post-add/post-sub carry test on
  SLJIT). `mul_wide_*` uses portable 32-bit-limb multiplication, as NUM-1
  requires, rather than `__int128`.
* **The unit test used the register ABI for a frame-ABI module.** Both backends
  select the staging frame for these multi-result signatures; `call2` now checks
  `frameCallAbi()` and marshals through `callCallableFrame`.

### Evidence

```
SLJIT  memory64 size/grow  15/15
AsmJIT memory64 size/grow  15/15
SLJIT  wide arithmetic     27/27
AsmJIT wide arithmetic     27/27
```

Full serial runs:

```
SLJIT   821 cases: 780 passed / 41 failed; no signal
AsmJIT  821 cases: 776 passed / 45 failed; no signal
```

### Lessons

* **A 64-bit table needs 64-bit operands and results everywhere.** Reading the
  delta as 4 bytes was enough to turn `-1` into `1`.
* **Keep both operands alive across a two-call operation.** SLJIT's first
  `mul_wide` draft overwrote the left operand with the low result, so the high
  helper multiplied the low half by the right operand.


## Stringrefs — shared string helpers and the backend transport

### Intended

Make stringref instructions a backend-neutral core capability rather than a
LibJIT-only slice: constants, measures, equality, concat, linear-memory and
GC-array constructors/encoders.

### Root cause and fix

The string semantics existed only as static helpers inside the LibJIT
dispatcher, so every other backend refused the opcodes. They now live in
`WasmStringOps`, exported by the core:

* `string.const` embeds the instance's materialised literal pointer.
* measures, `eq`, `concat`, `is_usv_sequence` are shared helpers.
* `string.new_*` / `string.encode_*` take an explicit memory index, so a backend
  never resolves or pins a `LinearMemory*` itself.
* GC-array constructors/encoders use a packed range/layout form, keeping the
  helper ABI at four arguments on SLJIT. The layout carries data offset,
  element size and codec mode; the range carries `start` low, `end` high.

SLJIT and AsmJIT now dispatch that family; the eight array variants and the
memory constructors/encoders are green.

### Evidence

```
SLJIT  string.const, measure, eq, concat   green
AsmJIT string.const, measure, eq, concat   green
SLJIT  string.new_utf8 strict/lossy        9/9
AsmJIT string.new_utf8 strict/lossy        9/9
SLJIT  string array constructors/encoders  21/21
AsmJIT string array constructors/encoders  21/21
```

Full serial runs:

```
SLJIT   821 cases: 784 passed / 37 failed; no signal
AsmJIT  821 cases: 780 passed / 41 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **String semantics are core, not backend.** A backend should transport handles
  and call one shared implementation; duplicating codecs per backend would drift.
* **Pack only at the ABI boundary, and document the word order.** The array bridge
  needs five logical fields but SLJIT has four argument registers; the packed
  range and layout are part of the helper contract now.


## Bounded recursion on SLJIT

### Fix

The SLJIT frame always reserved 128 staging slots and the continuation frame,
so a trivial recursive function needed several kilobytes per Wasm frame and
10,000 frames crossed the 512 KiB reserve. Measurement now records whether the
body uses a frame-form call or a continuation operation; the staging area (128
slots) and continuation frame are reserved only then. Two register-form call
out slots are always reserved for a v128 result, and operations that build a
descriptor in the call area (`array.init_*`, `table.fill`, `table.init`) mark
the staging area as needed during measurement.

### Evidence

```
SLJIT  unbounded recursion + 10k bounded descent   6/6
SLJIT  stack-switching                             90/90
SLJIT  multi-table                                 21/21
SLJIT  full serial                                 821 cases: 814 passed / 7 failed
Default full                                        22,451 assertions in 821 cases, green
```

### Lessons

* **The call area has two consumers with different lifetimes**: staging for a
  frame-form call and a two-slot out-buffer for a register-form v128 result.
  Removing staging must keep the latter.
* **A descriptor write is a frame-size dependency even though it is not a
  call.** `table.fill`/`table.init`/`array.init_*` write past the operand stack,
  so they must mark staging during measurement, not only during emission.



### Fix

* Moved the exact (`long double`) rounding semantics into the shared
  `WasmRoundingOps` C helpers: f32/f64 unary and binary arithmetic, and the
  ten int/float conversions, each taking the raw sub-opcode.
* Both backends lower the single `dispatchRoundingVariant` hook by selecting
  the helper and operand/result widths from `(sub - 0x80) % 20`.
* Added the new public header to the backend tier and its compile-only TU.

### Evidence

```
SLJIT   rounding variants   13/13
AsmJIT  rounding variants   13/13
Default 22,451 assertions in 821 cases, green
```

Full serial runs:

```
SLJIT   821 cases: 813 passed / 8 failed
AsmJIT  821 cases: 809 passed / 12 failed
```

### Lessons

* **Round from the exact intermediate, not the host FP mode.** Casting before
  applying the requested direction double-rounds values such as 16777217.
* **A new public header has two obligations**: a tier entry and a
  compile-only TU. The `[tiers]` count must be bumped with it.



### Fix

AsmJIT's `pushControlFrame` rejected a block whose declared parameters had no
live producers. Relaxed dead-code validation permits exactly that after a
terminator: the block is polymorphic and never executes. In an already
terminated arm the frame now uses placeholder registers instead of failing.

### Evidence

```
AsmJIT  relaxed dead-code validation   19/19
AsmJIT  full serial                    821 cases: 808 passed / 13 failed
```

### Lesson

* **Dead code still needs a well-formed compile-time frame.** The frame's
  operands may be placeholders; rejecting the underflow defeats the relaxed
  policy while accepting it in a live arm would still be a bug.



### Fix

* Lowered `array.new_array`, `array.new_memory` and `array.new_table` on both
  backends through the existing `ModuleInstance` runtime methods.
* The helper ABI packs destination/source type indices into one 64-bit word and
  the `(start, length)` range into another. The runtime performs the GC range
  check before allocating.

### Evidence

```
SLJIT   more array constructors   24/24
AsmJIT  more array constructors   24/24
```

Full serial runs:

```
SLJIT   821 cases: 812 passed / 9 failed
AsmJIT  821 cases: 807 passed / 14 failed
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **The range-word order is part of the helper contract.** A reversed pack
  produced the right element count by accident and the wrong slice contents.
* **SLJIT scratch registers are scarce.** Building two packed words without a
  fifth register is possible by reusing the context register before the final
  `loadContext`.



### Intended

Complete the generated-code side of the continuation family.

### Fix

* `resume` now routes a `Suspended` status to its `on $tag` clauses: it
  compares the runtime tag id, pushes the tag payload plus replacement
  continuation, and branches to the target label. An unhandled suspension
  releases the replacement handle and reports
  `"unhandled continuation suspension"`.
* SLJIT models the handler stack in the measuring pass so the fixed frame is
  large enough.
* `resume_throw` and `resume_throw_ref` construct the injected exception and
  feed it through the same resume path via `ContinuationOpFrame::exception`.
* `switch` passes the peer argument prefix, drops the target handle and pushes
  the values returned when the peer switches back.
* Ordinary `drop` now releases values that name continuation handles through a
  registry-safe helper, and reference locals are released on function return.

### Evidence

Each test passes in isolation on both backends:

```
stack-switching module routes a suspension to its handler
stack-switching module drops a suspension with no handler
stack-switching binds a suspended continuation
stack-switching dispatches multiple handlers and a loop handler
stack-switching suspends with a wide payload
stack-switching injects an exception into a suspended continuation
stack-switching resume_throw dispatches a handler clause
stack-switching switch performs a symmetric native-stack handoff
stack-switching switch performs a symmetric host-thread handoff
```

### Aggregate verification

The whole `stack-switching*` filter is green on both backends: **90 assertions in
15 test cases**.

### Final bug: runtime tag id vs module-local tag index

The first aggregate run failed only when a module with another tag had been
instantiated first. `wasm_cont_suspend_ops` was receiving the runtime `TagId`
and also using it to index the module-local tag section for the payload type.
Those are different namespaces. The helper now takes both in one packed word:
low 32 bits module-local index, high 32 bits runtime id.

### Lessons

* **A tag has two identities.** The module-local index drives decoding and type
  lookup; the runtime `TagId` drives matching. Never use one for the other.
* Individual test success does not prove namespace correctness; a preceding
  module that allocates another tag exposes the confusion.




### Intended

Give both new backends a real continuation entry path instead of the generic
“opcode not lowered” failure.

### Fix

* Added `ContinuationOpFrame`, the fixed backend scratch frame shared with the
  existing `ContinuationValue` ABI.
* Added a libffi-backed `ContinuationInvokeFn` in the core. It understands both
  the register ABI and the staging-frame ABI, so a continuation target compiled
  by either backend can be invoked from the provider worker.
* Added `wasm_cont_new_ops`, `wasm_cont_resume_ops`, `wasm_cont_bind_ops` and
  `wasm_cont_suspend_ops`; the first three are lowered on both backends.
* SLJIT reserves one fixed `ContinuationOpFrame` in its measured frame.
  AsmJIT allocates one with `new_stack` per operation.
* The wide/multi-value test now honours `frameCallAbi()` and passes reference
  arguments/results in the canonical frame slots.

### Evidence

Green on both backends:

```
stack-switching module runs cont.new and resume
stack-switching module passes an argument through resume
stack-switching carries multi-value and reference payloads
stack-switching binds a continuation argument prefix
```

Full serial runs:

```
SLJIT   821 cases: 802 passed / 19 failed; no signal
AsmJIT  821 cases: 797 passed / 24 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Remaining in the family

Suspension is lowered, but `resume` still treats a `Suspended` status as an
unhandled trap. The next piece is the handler-clause control flow: branch to the
`on $tag` label with the tag payload plus the replacement continuation, release
an unhandled suspension, and implement `resume_throw` / `switch`.

### Lessons

* **The invoker must know the target's module ABI.** A continuation worker is a
  different stack but the same `Callable`; calling it with the wrong ABI corrupts
  the worker immediately.
* **`ContinuationValueKind` and `ValueKind` are different enums.** They happen to
  have overlapping spellings but different numeric values; converting by cast
  silently shifted every kind.



### Intended

Make externref/host-reference counts survive `global.set`, `global.get` and
reference `local.get`, including the host bridge callbacks.

### Fix

* Added `wasm_global_set_ref` (a `ModuleInstance::refStore` write barrier),
  `wasm_ref_clone` and `wasm_ref_drop`.
* `global.set` on a reference calls the write barrier and then releases the
  operand stack's count.
* `global.get` and reference `local.get` clone the loaded reference, so the
  operand stack owns its own count.
* AsmJIT now records each local's declared kind for the barrier decision;
  SLJIT already had `localKinds_`.
* Both backends implement the reference arm of `global.set` instead of a raw
  8-byte store.

### Evidence

```
SLJIT   host bridge reference callbacks   17/17
AsmJIT  host bridge reference callbacks   17/17
SLJIT   native imports round-trip refs    19/19
AsmJIT  native imports round-trip refs    19/19
```

Full serial runs:

```
SLJIT   821 cases: 796 passed / 25 failed; no signal
AsmJIT  821 cases: 791 passed / 30 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **A global reference cell is a barrier slot, not an `i64`.** A raw store
  silently breaks host counts even when every value is otherwise correct.
* **Loads from a global/local borrow; the operand stack needs an owned clone.**
  Without that, a later `drop` or store would release somebody else's count.



### Intended

Make a discarded `string.concat` result release its `StringStore` object, not
just disappear from the operand stack.

### Fix

* Operation results (`concat`, `new_*`, GC-array constructors, view/iterator
  factories and slices) now arrive retained: `makeString` and `makeStringView`
  take one ownership count, and `concat` goes through the same path.
* `wasm_string_release` is the shared release helper.
* SLJIT carries a `stackRefKinds_` shadow beside `stackWidths_`; every marked
  producer sets the top bit and `drop` releases only when that bit is set.
* AsmJIT records the virtual-register ids of owned string results in
  `stringValues`; `drop` releases only a tracked id. Virtual ids are unique, so
  a stale entry cannot match a later value.
* `string.const` deliberately pushes a borrowed literal, so dropping it does not
  release the instance-owned literal.

### Evidence

```
SLJIT   a discarded string concat result   6/6
AsmJIT  a discarded string concat result   6/6
```

Full serial runs:

```
SLJIT   821 cases: 794 passed / 27 failed; no signal
AsmJIT  821 cases: 789 passed / 32 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **`drop` is type-directed even though the opcode is polymorphic.** A backend
  needs an ownership side-table beside the operand stack; treating every word as
  a reference would corrupt `i64` values.
* **Producers must create owned values.** Releasing an unowned `StringStore`
  object is a no-op, so retention belongs on the producing helper.



### Intended

Close the whole STR-4 stringview facility on both new backends.

### Fix

* Moved the LibJIT-local view semantics into `WasmStringOps`: view/iterator
  creation, WTF-8 advance/encode/slice, WTF-16 length/get/encode/slice, and the
  iterator next/advance/rewind/slice operations.
* The helper ABI is packed for SLJIT's four argument registers:
  `packedMemory = memoryIndex | pointer << 8` and
  `positionAndBytes = position | count << 32`; the WTF-8 encoder returns
  `nextPosition << 32 | written`.
* Both backends lower the seventeen dispatch hooks, and stringview/iterator
  value types now count as one-word reference locals in both width models.
* The stringview test now honours `frameCallAbi()`, as the existing multi-value
  ABI test already did: a multi-result module compiled by SLJIT/AsmJIT uses
  8-byte result slots, while the public tuple is packed.

### Evidence

```
SLJIT   stringview*  37 assertions / 2 cases, green
AsmJIT  stringview*  37 assertions / 2 cases, green
Default stringview*  37 assertions / 2 cases, green
```

Full serial runs:

```
SLJIT   821 cases: 793 passed / 28 failed; no signal
AsmJIT  821 cases: 788 passed / 33 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **Reference types are one-word locals in every backend**, even when their
  operations are specialised; leaving them out of the width model turns a
  perfectly valid function into a compile failure.
* **A multi-result test must branch on the module ABI.** SLJIT and AsmJIT choose
  the staging frame for a multi-result module; LibJIT's register out-buffer has a
  different result layout.



### Intended

Close the three smallest remaining common feature failures.

### Fix

* **`i32.itt`** is a compile-time-only marker. Both dispatchers now call
  `traceImmediate`; SLJIT reports it only from the emitting pass so the measure
  pass does not double the callback.
* **`memory.discard`** moved into `WasmMemory` as `wasm_memory_discard`. It checks
  the range before touching a page, zero-fills page-rounded ranges and uses the
  provider's discard hint when available. Both backends widen i32 operands to the
  helper's 64-bit ABI.
* **Trap labels**: `unreachable` now reports the interned
  `"unreachable in function N ($name)"` detail, matching LibJIT. AsmJIT passes the
  pointer as an argument; SLJIT embeds it. SLJIT's emit-only guard also avoids
  emitting a pointer immediate during the measuring pass.

### Evidence

```
SLJIT  i32.itt                    6/6
AsmJIT i32.itt                    6/6
SLJIT  memory.discard            24/24
AsmJIT memory.discard            24/24
SLJIT  trap messages name ...     7/7
AsmJIT trap messages name ...     7/7
```

Full serial runs:

```
SLJIT   821 cases: 789 passed / 32 failed; no signal
AsmJIT  821 cases: 784 passed / 37 failed; no signal
Default 22,448 assertions in 821 cases, green
```

### Lessons

* **A measure pass has no code buffer.** An immediate emission added for the
  emitting pass must be guarded, even in a dispatcher that previously emitted no
  code.
* **Diagnostic text is part of the backend contract.** The trap name must be
  composed and interned the same way the reference backend does it.
