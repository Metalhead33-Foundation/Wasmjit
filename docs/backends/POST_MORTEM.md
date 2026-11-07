# Remaining-work post-mortem (2026-11-07)

This note is the mandatory record of the "what remains" work, item by item, in
the order of `HANDOFF.md` section 7. Each section records what was intended, what
actually happened, the root cause of every surprise, and what is still open.

## Status at the end of this pass

| # | Item | Status | Evidence / blocker |
|---|---|---|---|
| 1 | constant-stack tail calls | **done for every lowered form** | self, direct, indirect and frame (scalar and v128) chains go through the libffi driver (`TAILSELF`/`TAILMUTUAL`/`TAILINDIRECT`/`TAILFRAME`/`TAILV128FRAME`, 1,000,000 links, `run()=1`); `ref` is not lowered by either backend |
| 2 | shared reference/table atomics | **blocked** | numeric family done; the parser rejects shared composite references (`malformed mutability`), table atomics need `table.get`/`table.set`, and SLJIT's four-argument ceiling needs a record for `cmpxchg` |
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

### What is still open

**Mutual and indirect tail calls still use call + return.** A cross-function
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

* `return_call_ref` is not lowered by SLJIT or AsmJIT at all (neither has a
  `call_ref` lowering), so there is no backend path to extend yet. When one is
  added, the runtime `tailEntry` check above is the pattern.
### v128 frame-form targets (same session)

`tailCallFrameSupported` now accepts `v128`: a uniform slot is 16 bytes, the frame
thunk copies `frameValueBytes(v128)`, and both backends store the vector into the
slot before the pending write.

* SLJIT copies the two 8-byte halves from the operand slot to `TailCallState::args`.
* AsmJIT's register model keeps a v128 as a buffer pointer; the pending writer
  loads 16 bytes with `movdqu` and stores them into the state slot.

Evidence (`tailcall_v128_frame_probe.wat`, three vectors and a scalar counter,
1,000,000 links):

```
TAILV128FRAME asmjit: run()=1
TAILV128FRAME sljit: run()=1
```

The pre-existing `tail_wide` one-link probe now takes the trampoline as well
(`tail_wide=1`). LibJIT is excluded from the v128 probe: its native trampoline has
never carried `v128` (docs/TAILCALLS.md TC-3).

### What is still open

* `return_call_ref` is not lowered by SLJIT or AsmJIT at all (neither has a
  `call_ref` lowering), so there is no backend path to extend yet. When one is
  added, the runtime `tailEntry` check above is the pattern.

## Item 2 — shared reference/table atomics

### Intended

Extend the numeric shared-everything atomics to reference slots (globals, struct
fields, array elements) and table slots, with the GC retain/release barrier.

### What we found (blockers, not a completion)

* The numeric family is complete on all three backends (previous commit).
* **The core parser rejects the only valid upstream fixture.** A shared struct
  with `(ref null (shared any))` validates with the pinned `wasm-tools`, but
  `Runtime::loadFile` fails on all three backends with *"malformed mutability"*.
  The type model does not carry shared composite references yet, so no atomic
  lowering can be exercised. `shared_ref_atomic_probe.wat` is kept as a valid
  upstream fixture and a documented refusal; its round-trip assertion starts
  running when the parser accepts it.

* **Reference slots in a shared composite are not expressible in the pinned
  `wasm-tools`.** `struct.atomic.set` on `anyref` is rejected; `externref`,
  `funcref`, `eqref`, `structref`, `arrayref`, `i31ref` and the null types are
  rejected too. The only combination that validates is a **shared** struct whose
  field is `(ref null (shared any))`, i.e. the shared-everything reference type.
  A fixture for it therefore needs shared-composite parsing and shared-reference
  type identity, not just an atomic opcode.
* **Table slots have no `table.get`/`table.set` lowering on SLJIT/AsmJIT at
  all**, so a table-atomic row cannot be reached without first implementing the
  ordinary table accessors.
* SLJIT's four-argument C-call ceiling cannot express
  `wasm_atomic_ref_cmpxchg(ctx, address, expected, replacement, ordering)`
  (five arguments). It needs either a packed stack record (like the existing
  numeric cmpxchg selector) or a core wrapper with the ordering folded into the
  selector.

### What is still open

Shared-reference atomics on globals/structs/arrays, and table atomics (which
first need `table.get`/`table.set`). The numeric family is not affected. This is
recorded as a blocker, not claimed as done.

### Lessons

* **"The opcode is in the proposal" is not the same as "the type is
  expressible."** Always validate the *fixture* with the pinned tool before
  designing the lowering.
* **A prerequisite hidden behind a feature is still a prerequisite.** Table
  atomics need table accessors; there is no shortcut through the atomic helper.

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
