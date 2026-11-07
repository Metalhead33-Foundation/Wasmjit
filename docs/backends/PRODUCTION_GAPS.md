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

## 7. 2b for AsmJIT is a migration; the dead branch has exact coordinates

### AsmJIT cannot simply "record instead of throw"

`AsmjitModuleCompiler.cpp` contains **191** `throw std::runtime_error` sites, and two
tests in `AsmjitBackendTests.cpp` assert on those throws (`REQUIRE_THROWS`). Converting
refusals from thrown errors into recorded reasons would therefore:

* change instantiation from "fails loudly" to "succeeds with a missing export" for
every one of those shapes at once;
* break the two tests that pin the current behaviour;
* blur the distinction callers already rely on (`throw` = this module is unusable,
  recorded reason = this *function* is unusable).

So 2b for AsmJIT is a **semantic migration**, and it should be scoped deliberately:
record reasons for the shapes that are *per-function* (unsupported signature, body
shape) and keep throwing for module-level failures. That is a design decision first.

LibJIT is the mirror image: its refusals are module-level `std::runtime_error`s raised
before codegen, so "record" does not apply there either -- it needs *function-level*
reasons, which means moving the checks from the module scan into the per-function
compile.

### The dead epilogue branch: exact coordinates

```
:1097  } else if (frameMode) {   <- LIVE  (comment: "steps 5c/5e")
:1124  } else if (bufferedVectorResult) {
:1141  } else if (frameMode) {   <- DEAD  (comment: "step 5c"), unreachable
```

Both branches open with the **identical text** `} else if (frameMode) {`, so a
`old_text` replacement cannot target the second one -- and the leading `}` must be
kept, because it closes the `bufferedVectorResult` branch. The deletion is therefore a
**ranged** edit:

* keep the `}` that opens line 1141;
* delete from ` else if (frameMode) {` through the matching `}` at the end of that
  branch's body;
* the following `else` (the `ret(values.back())` fallback) then attaches to
  `bufferedVectorResult`, which is the correct chain.

Verified by the grep above; not applied here because the body is ~27 lines and a
mis-matched brace would compile silently into a wrong branch chain -- the one failure
mode this campaign has learned to refuse.

Suite 21862 assertions in 776 cases, green.

## 8. Decision: AsmJIT fails the whole module (2b closed by policy)

The project owner's ruling:

> **Module-level throw. If one function fails, the entire module is garbage.**

That settles 2b for AsmJIT in one line -- **there is nothing to implement**. AsmJIT
throwing from the dispatcher is not a limitation of the reporting mechanism; it is the
declared contract:

| Backend | A refused function means | `unsupportedReason` |
|---|---|---|
| **SLJIT** | *that function* is uncallable; the module is otherwise usable | **populated** (2b landed) |
| **AsmJIT** | the **module** is unusable -- instantiation throws | never populated, by design |
| **LibJIT** | module-level refusals today; per-function once the checks move into the compiler | pending |

The two models are coherent rather than contradictory: SLJIT can degrade to a partial
module because its failures are per-function; AsmJIT's failures are structural (ABI
translation, unsupported value types), so a partial module would be a lie. **A tree that
sometimes throws and sometimes reports must say which is which** -- and it now does.

## 9. Housekeeping 3a done: the dead epilogue branch is gone

```
before:  :1097 live "steps 5c/5e"   :1124 bufferedVectorResult   :1141 dead "step 5c"
after:   :1097 live                :1124 bufferedVectorResult   (no dead branch)
```

Applied as a **ranged** edit, exactly as section 7 specified: keep the `}` that closes
`bufferedVectorResult`, drop ` else if (frameMode) {` through the dead body's closing
brace, and let the fallback `else` attach to the branch above. Verified by count
(two occurrences of the marker became one), by the compiler, and by both suites:
step-5 gates **31 in 7**, whole suite **21862 in 776**.

**Why the script and not the editor:** the two branches opened with identical text, and
a partial match would have deleted the *live* one -- which would still have compiled,
because the dead one handled the same case. That is the silent-branch-chain failure
this campaign has refused three times; the script asserted `keep < a2 < resume` before
writing.

## 10. Item A, step 1: the measurement exists (still inert)

```cpp
static bool TypeTranslator::signatureNeedsFrame(const WASM::FuncType& function);
```

True when a signature cannot be delivered by the register form:

* any `v128` parameter -- two words, spelled as an aggregate by the register path;
* more than one result;
* a `v128` result -- it needs a caller buffer today;
* more argument entries than the C ABI call allows (`ctx` plus more than three).

**Behaviour is unchanged**: the module compiler still reads
`WASM::frameCallAbiOptIn()`, so nothing moves and no fixture shifts. That is the point
of step 1 -- the decision logic now exists, is documented, and can be reviewed on its
own merits.

### The two steps that remain for item A

| Step | Change | Churn |
|---|---|---|
| 2 | `frameMode = optIn \|\| any signature needs it` and fix the fixtures that move | **31 fixtures contain `v128`**, 16 references in `AsmjitBackendTests` |
| 3 | drop the opt-in for AsmJIT entirely once 2 is green | none |

For step 2 the useful measurement is now *mechanical*: compile the suite, list every
AsmJIT test whose fixture flipped to `frameCallAbi() == true`, and convert those to
`callCallableFrame`. The list is finite, and each conversion is the same edit.

### One recurring mechanical hazard, worth writing down

The editor tool doubles backslashes inside Doxygen commands (`\brief` arrives as
`\\brief`), and inserting a `/*! */` block above an already-documented declaration
makes Doxygen **merge the two docs onto one function**. Both happened here, and both
were caught by gates (`WARN_NO_PARAMDOC` and a build error). The fix pattern, now used
twice: normalise `\\` -> `\`, then move the older block down beside its own
declaration.

## 11. Item A step 2: the decision is per FUNCTION, not per module

Before flipping anything, the sites were read. There are three, and they do not share a
scope:

| Site | Scope | Today |
|---|---|---|
| `translateTypes` signature loop (`:5173`) | **per type** | `frameMode` from the opt-in |
| `FunctionCompiler` constructor local `frameAbi` (`:894`) | **per function** | same |
| `FunctionCompiler` member `frameMode` (`:5137`) | **per function** | same |

So "frame mode" is already consulted at three different granularities, and step 2's
"module-wide flag, or measurement" would have smeared one answer across all three.
The natural decision is **per signature**, computed by the helper added in step 1:

```cpp
translateFunctionType(type, TypeTranslator::signatureNeedsFrame(type) || optIn)
```

and the two `FunctionCompiler` sites read the same expression for their own `type`.
That is *smaller* than the plan and strictly more precise: a module with one wide
function and nine narrow ones stops paying the frame cost for all ten.

### The consequence that must be decided first

`ModuleInstance::frameCallAbi()` is **per module**, and the cache reads it as "this
instance's entries take `(ctx, frame)`". With per-function modes that stops being true:

* the flag can only mean *"at least one function is frame-form"*, which the cache would
  misread for the narrow ones;
* the honest fix is to make the frame-ness a property of the **`Callable`** (or of the
  cache entry), not of the instance -- and §39 records that `Callable` is frozen, so
  that is an API decision, not an edit.

**Therefore step 2 has a prerequisite the plan did not name:** decide where per-function
frame-ness lives before flipping the default. Options, cheapest first:

1. keep the mode **per module** (as today) and let the measurement apply only to
   modules that need it *entirely* -- no API change, and it still fixes the reported
   defect (AsmJIT's frame form depending on a global switch);
2. per function, with frame-ness carried by `Callable` -- correct, but an API decision.

Option 1 is what the plan intended and needs no new surface; option 2 is better and
should be scheduled deliberately. **Neither was flipped today**, because choosing wrong
would invalidate the 31-fixture migration that follows.

Suite 21866 assertions in 777 cases, green; nothing changed.
