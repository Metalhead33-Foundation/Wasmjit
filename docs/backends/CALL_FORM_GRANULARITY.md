# Call-form granularity: per module, per signature or per callable?

**Status:** living note, 2026-11-09 (measured snapshot, see §3).
**Companions:** [`STAGING_CALL_ABI.md`](STAGING_CALL_ABI.md) (the form itself),
[`EXPORT_SIGNATURE.md`](EXPORT_SIGNATURE.md) (what the frame changes at the
embedder seam), [`../ABI_PORTABILITY.md`](../ABI_PORTABILITY.md) (the canonical
signature every backend must honour), [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md)
(the typed entry points that already hide the form).

**The question this note answers.** "Should the register-or-frame choice be
recorded per *function* rather than per *module*, with SLJIT and AsmJIT defaulting
to the frame form and LibJIT to the register form, wrapped so the embedder only
ever sees `Ret fn(VMContext*, Args…)`?"

**Short answer.** The diagnosis is right and the wrapper already exists; the
*default* is wrong, and the granularity that actually removes the defect is the
**signature**, not the function. §1 separates the three claims, §2 gives the
recommendation, §3 the evidence, §4 the cost of doing it, §5 what is deliberately
*not* proposed.

## 1. The proposal has three parts, and only two are sound

| Part | Verdict | Why |
|---|---|---|
| Record the form below module granularity | **Agree** — but per *signature*, not per function | The form exists because of a **signature ceiling** (SLJIT 4 argument entries, AsmJIT 4, LibJIT's six-word loss). It is a pure function of the signature plus the backend's ceiling, so two functions with the same type must have the same form. Recording it per function would let them differ, which is a new way to be wrong. |
| SLJIT/AsmJIT use the frame form **by default** | **Disagree** | The frame costs one store per argument at the caller and one load per parameter in the callee, and buys nothing unless the register form cannot express the signature. It also changes `v128`-result semantics (register form: caller buffer; frame form: result block) and forfeits the register-form tail-call trampoline for every function in the module. `EXPORT_SIGNATURE.md` §4 already argued this; nothing has changed. |
| A wrapper so the embedder never sees a frame | **Already true** | `ModuleCallCache` marshals typed arguments into a frame for the embedder (`WasmCallCache.cpp`, the `frameCallAbi()` branch), and `WASM::callCallable<Ret>` is literally `Ret fn(VMContext*, Args…)`. A second template wrapper would duplicate it. The *inverse* direction — native C++ exposed to wasm — is a `Callable` with a native `fnPtr` and a null `context`, which every backend calls in the register form by construction. |

## 2. Recommendation

1. **Keep the rule "frame only when the register form cannot express the
   signature", and move the decision from the module to the signature.** One
   predicate, owned by `WasmBase`, parameterised by a backend-declared ceiling:
   `usesFrameForm(signature, ceiling, optIn)`. SLJIT and AsmJIT then give the
   frame only to the wide signatures; LibJIT keeps the register form and takes
   the frame on opt-in, as today.
2. **Declare the ceiling through the backend capability surface** rather than
   having each backend re-derive a module-wide flag. That is what makes the rule
   backend-neutral (`WasmBase/` must not name a backend) and keeps one copy of it.
3. **Keep `setFrameCallAbiOptIn(true)`** as the "one form everywhere" escape
   hatch, because a harness or an embedder may want a single ABI; it is a
   canonicalisation request, not an inference.
4. **Treat this as its own milestone**, not as a follow-up to the reference work:
   it changes the *behaviour* of a frozen surface (`Callable`, the export entry,
   the continuation invoke path) even though no header's shape changes, and
   `API_ABI_STABILITY.md` requires a milestone decision for that.

## 3. Why the module-wide rule is a defect, not a preference

**Snapshot (2026-11-09, working tree):** all three backends compile and run the
whole suite identically (22,482 assertions in 822 cases each), so the defects
below are *not* observed by the suite. They are read from the code and recorded
here so the next session measures them rather than rediscovering them.

| Defect | Where | Consequence |
|---|---|---|
| **Contagion.** One wide function switches every function in the module into the frame form. | `SljitModuleCompiler.cpp` (`frameMode` loop); `AsmjitTypeTranslator.cpp` (`moduleNeedsFrame`) | Every call in that module pays the frame's stores and loads, and `tailCallTrampolinesEligible(module, frameMode)` is false for the whole module, so a module with one wide function loses the register-form tail-call trampoline for its narrow ones too. |
| **Cross-module unsoundness.** A register-form module that imports a function from a frame-form module emits a register-form call into a frame-form entry. | The import path (`emitCall` → `Callable::context` → direct call) uses the *caller's* form; nothing compares the callee's. | Silent corruption, not a trap. `WasmRuntime::instantiate` guards only the re-instantiation case (`instance->frameCallAbi() != frameCallAbiOptIn()`), and `WasmComponentRuntime` derives the form from the callee's instance (`callableUsesFrameAbi`) for its own path — the plain module import path has no such check. **A per-signature rule removes this class by construction**: two modules that agree on a type agree on its form. |

Both defects disappear under the §2 rule; neither is fixed by recording the form
per function *and* keeping module-wide defaults.

## 4. What it costs

| Work item | Where | Note |
|---|---|---|
| The shared predicate + the capability field | `WasmBackendAbi.hpp`, `WasmBackendSession.hpp`, the three backends' capability declarations | Small, and it is the piece that keeps the rule backend-neutral. |
| Per-function entry form | each backend's module compiler and body dispatcher | The `frameAbi_` reads in the body are already per function (a constructor argument); what changes is who decides it. |
| Call sites | each backend's call emitters | A direct call's form is static (the callee's type is known). `call_indirect`/`call_ref` need a **runtime** branch or a per-type thunk, because the callee is only known at run time — this is the real cost, and the reason not to do it casually. |
| Tail-call thunks | `WasmTailTrampoline.cpp` and each backend | One thunk per (type, form) instead of per type. |
| Embedder seam | `WasmCallCache.cpp`, `WasmContinuation.cpp`, `WasmComponentRuntime.cpp` | Each already has (or can derive) the form per callee; the module-wide boolean becomes a query. |
| Tests | a two-module fixture: register-form module A imports a *narrow* function from module B, which is frame-form because of an *unrelated wide* function | This is the fixture that fails today and must pass afterwards. |

Rough size: a focused multi-session slab, not an afternoon. That is the honest
reason it is not part of this change: it moves references between frames, and the
reference-ownership work that *was* done here is its prerequisite (a form switch
must not move ownership).

## 5. What is deliberately not proposed

* **Frame by default on any backend.** It inverts `EXPORT_SIGNATURE.md` §4, pays a
  cost on every call to buy nothing, and silently changes the ABI of modules that
  already run — precisely what `setFrameCallAbiOptIn` exists to prevent.
* **A second C++ wrapper template.** `ModuleCallCache` already is the one
  marshaller; two would drift.
* **Inferring the form from measurements.** Already forbidden
  (`EXPORT_SIGNATURE.md` §4): inference broke ten working modules when it was
  tried.
* **Mixing backends in one call graph.** Still refused by review
  (`EXPORT_SIGNATURE.md` §6): each backend's frame path is pinned only by its own
  tests.
