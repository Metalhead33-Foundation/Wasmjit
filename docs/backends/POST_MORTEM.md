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
