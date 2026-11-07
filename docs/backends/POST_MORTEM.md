# Remaining-work post-mortem (2026-11-07)

This note is the mandatory record of the "what remains" work, item by item, in
the order of `HANDOFF.md` section 7. Each section records what was intended, what
actually happened, the root cause of every surprise, and what is still open.

## Status at the end of this pass

| # | Item | Status | Evidence / blocker |
|---|---|---|---|
| 1 | constant-stack tail calls | **done for every lowered form** | self, direct, indirect and frame (scalar and v128) chains go through the libffi driver (`TAILSELF`/`TAILMUTUAL`/`TAILINDIRECT`/`TAILFRAME`/`TAILV128FRAME`, 1,000,000 links, `run()=1`); `ref` is not lowered by either backend |
| 2 | shared reference/table atomics | **done** | numeric, struct/array reference and shared-table atomics run on all three, and so do global `eqref` atomics (`SHAREDREF`, `TABLEATOMIC`, `GLOBALREF`) |
| 3 | AsmJIT block parameters | **done** | `BLOCKPARAM` block=42 if=11/12 loop=0 on all three |
| 4 | bulk memory on memory64 | **done** | `MEM64BULK` copy=97 fill=65 init=97 on all three |

Suite at the last commit: **22184 assertions in 798 cases** under
`--rng-seed 12345`, green; `[headerdoc]`, `[docs]` and `doxygen` are warning-free.

The two open rows are the honest remainder. Item 1 is a cross-function driver/thunk
problem, not a local lowering; item 2 is blocked on a prerequisite (table accessors) and
on a fixture shape the pinned tool accepts.

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

### What is still open

No reference/table atomic shape in the tested proposal surface is left. Shared
`v128` slots are outside the atomic proposal and remain refused by the numeric
width check.

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
