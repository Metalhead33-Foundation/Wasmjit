# Production-readiness gaps, backend by backend

**Status:** living note, 2026-11-06 (`backendsMh33`). Companions:
[`EXPORT_SIGNATURE.md`](EXPORT_SIGNATURE.md) (the ABI itself),
[`STAGING_CALL_ABI.md`](STAGING_CALL_ABI.md) (its history).

This note exists because a survey of refusal strings (EXPORT_SIGNATURE §44) described
"limits" that are **not limits**: the frame form exists precisely to remove them.
That survey was misleading, and the correction is the first section here.

## 1. The frame form is not opt-in where it is *needed*

The intended policy, restated:

| Backend | Frame form | Why |
|---|---|---|
| **SLJIT** | **automatic** when the signature exceeds the register form | the four-entry ceiling is hard |
| **AsmJIT** | **should be automatic** for the same reason (vectors, wide signatures) | the four-entry ceiling is hard |
| **LibJIT** | **opt-in**, register form by default | libjit has a real allocator; the register form is the fastest thing it does |

**SLJIT implements this** (`frameAbi_` is derived from the saved-register count).
**AsmJIT does not yet**, and that is a defect, not a design choice:

"`cpp
// Asmjit/AsmjitModuleCompiler.cpp -- translateTypes
const bool frameMode = WASM::frameCallAbiOptIn();      // <- global switch, not a decision
"`

Because of this, an AsmJIT module with three `v128` parameters only works when the
*process-wide* opt-in happens to be set. That is the wrong shape twice over: it makes
a per-signature property depend on global state, and it contradicts the policy table.

**Next step (small):** choose the mode by measurement in `translateTypes` -- frame for
any signature carrying a `v128`, or needing more than `kArgumentSlotCeiling` argument
slots -- and keep the global flag only for LibJIT, where it is the stated policy.

### Consequences for the survey

* "SLJIT at most three parameters" and "AsmJIT too many arguments" are **register-form
  facts**. Neither is a production limit, and neither should appear in a gap list.
* The register form itself still exists and must stay correct -- narrow modules are
  byte-for-byte unchanged -- but it must never be the *only* form a shape can use.

## 2. Real gaps

### 2.1 Tail calls in frame mode -- REFUSED, and that is a gap

SLJIT refuses a tail call in frame mode because the trampoline does not carry a frame
pointer; AsmJIT and LibJIT have no frame-mode tail path at all.

**Impact:** a module using `return_call` / `return_call_indirect` silently falls back to
the register form on SLJIT (and to a refusal once the frame is mandatory), which for a
wide signature means the module is simply unsupported.

**Next step:** give the trampoline a frame pointer -- the thunk already takes five
arguments, so a sixth is the least invasive shape -- or state explicitly that tail
calls force the register form and refuse wide tail-calling modules.

### 2.2 Reference and GC types on SLJIT and AsmJIT -- MISSING

Neither backend advertises reference types in its ABI translation
(`AsmJit ABI translation: unsupported Wasm value type`, and SLJIT's
"cannot pass ..."), while LibJIT treats them as ordinary values.

**Impact:** `externref`, `funcref`, `anyref`, structs and arrays are **LibJIT-only**.
An embedder using GC is locked to one backend, which defeats the point of having three.

**Next step:** start with `externref` as a pointer-sized cell in the frame (8 bytes,
no GC interaction), then `funcref` (a `Callable*`), then struct/array references. Each
is a frame slot; the work is in the *body* lowering, not the boundary.

### 2.3 Exceptions -- NO REFUSAL FOUND, which is worse than a refusal

No refusal string for tags, `try_table` or `throw` exists in SLJIT or AsmJIT. Silence
means either support or an unhandled path, and **silence is not support**. The project
requires support here.

**Next step, in order:**

1. a fixture that only *declares* a tag and traps through it, run on all three
   backends, recording pass / refuse / trap -- this converts silence into a fact;
2. if it traps, a module-level refusal (the `unsupportedReason` mechanism already
   exists for exactly this);
3. then the real implementation, scoped per backend.

### 2.4 Multi-result in frame-mode LibJIT -- DEFERRED, with the refusal as the contract

Low priority for the reasons in EXPORT_SIGNATURE §39: the register form is LibJIT's
default and is sound (measured), the frame form refuses cleanly, and its only customer
would be cross-backend parity, which is forbidden in production.

## 3. Housekeeping and diagnosability

| Item | State |
|---|---|
| `unsupportedReason(name)` API | **landed** (EXPORT_SIGNATURE §43) |
| backends reporting into it | **pending** (2b): SLJIT computes the text already |
| dead duplicate `frameMode` branch in the AsmJIT epilogue | present, unreachable |
| compile cache keyed by mode | not done; makes the opt-in honest (§19) |

## 4. What this note does NOT claim

It does not claim the gaps are small. Reference/GC types and exceptions are the two
largest bodies of work left, and both are per-backend *body* work rather than ABI work --
the opposite of what this campaign spent its time on. What the campaign bought is that
the *boundary* no longer varies by backend, so those bodies of work can be done one
backend at a time without moving the contract.

## 5. 2b landed: SLJIT publishes its reasons

```
SLJIT reports why an export could not be compiled   [staging][sljit][abi][reason]  -> green
```

`SljitModuleCompiler` already collected the text in `unsupportedFunctions_`; a single
additional call publishes it per function:

```cpp
instance.setFunctionUnsupported(static_cast<std::uint32_t>(i),
        dispatcher.unsupportedReason());
```

The test asks a narrow two-result module (register form, where SLJIT has nowhere to put
the second result) and receives text containing **"multiple results"**. That is the
first time the tree answers *why* instead of merely failing -- section 34 refuted the
null-`fnPtr` inference, and this is the additive query that replaces it.

**Remaining for 2b:** AsmJIT (it `throw`s from the dispatcher rather than recording a
reason, so it needs the same "record instead of throw" treatment) and LibJIT (whose
refusals are module-level `std::runtime_error`s, which should also become recorded
reasons).

## 6. Item A is a migration, measured

Making AsmJIT decide the frame form by measurement is **not** a one-line change:
`Test/AsmjitBackendTests.cpp` references `v128` **16 times**, and **31 fixtures** under
`Test/wasm_wat/` contain `v128`. Flipping AsmJIT's default would move every one of them
to the frame form at once, and each has to keep passing.

**Plan, when taken on:**

1. land the measurement *behind* the existing flag first (flag OR measurement), so the
   code path exists and is exercised by the frame tests that already set the flag;
2. flip the default for **vectors only** -- the smallest shape AsmJIT cannot do without
   the frame -- and fix the fixtures that move;
3. then widen to argument counts.

Each step is green-or-revert on its own, which is the only way a 31-fixture migration
stays reviewable.
