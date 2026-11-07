# Remaining-work post-mortem (2026-11-07)

This note is the mandatory record of the "what remains" work, item by item, in
the order of `HANDOFF.md` section 7. Each section records what was intended, what
actually happened, the root cause of every surprise, and what is still open.

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

## Item 2 — shared reference/table atomics

### Intended

Extend the numeric shared-everything atomics to reference slots (globals, struct
fields, array elements) and table slots, with the GC retain/release barrier.

### What we found (blockers, not a completion)

* The numeric family is complete on all three backends (previous commit).
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
