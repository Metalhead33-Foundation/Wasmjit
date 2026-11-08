# Production-readiness gaps, backend by backend

**Status:** living note, 2026-11-07 (`backendsMh33`). Companions:
[`EXPORT_SIGNATURE.md`](EXPORT_SIGNATURE.md) (the ABI itself),
[`STAGING_CALL_ABI.md`](STAGING_CALL_ABI.md) (its history). **Section 27 is the current
curated capability ledger; section 29 is the production-readiness gap ledger**;
23-26 are history.

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
pointer; AsmJIT and LibJIT had no frame-mode tail path at all. LibJIT now lowers a
frame tail as a frame call followed by a frame return: correct, but not constant-stack.
A frame trampoline is still the missing piece for constant-stack frame tails.

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

### 2.4 Multi-result in frame-mode LibJIT -- IMPLEMENTED (2026-11-07)

The frame form now returns `void` and writes the whole result tuple into the caller's
result block; the module-level refusal is gone. The register form remains the default
and the frame form remains opt-in. See section 28.

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

## 12. Option 1 attempted: 3 lines, and exactly two tests block it

The flip was written -- `moduleNeedsFrame(module, optIn)` at the signature loop, the
constructor local and the member -- and the whole change is **three lines**. Measured
result: **777 cases, 775 passed, 3 assertions failed in 2 cases**, not the 31-fixture
migration the plan feared.

Both failures are *informative*, and only one of them is a pin:

| Test | What happened |
|---|---|
| `[asmjit][abi][v128][gap]` | my own gap pin, asserting that a `v128` parameter is **refused**. It is no longer refused -- the frame form handles it. The pin did exactly what it was for. |
| "AsmJit rolls back code allocations when a later function is unsupported" | a **safety** test. Its fixture is now `asmjit_brtable_multi_unsupported` (`br_table` to a multi-result function label), because the old v128 block-result probe became supported. |

The first is a one-line conversion to a positive test. The second is **not**: it guards
rollback of code allocations when instantiation fails, and the only way to keep that
guard is a fixture that is *still* unsupported. Deleting or weakening it to make the
flip pass would trade a safety property for a default.

### Status: helper landed, wiring reverted, tree green

* `TypeTranslator::signatureNeedsFrame(type)` and
  `TypeTranslator::moduleNeedsFrame(module, optIn)` are **committed and testable**;
* the three call sites still read the opt-in only, so behaviour is unchanged;
* suite back to 21866 assertions in 777 cases.

### What the flip needs before it can land

1. convert `[asmjit][abi][v128][gap]` to its positive form (assert `frameCallAbi()` is
   true and the export calls correctly) -- one test, mechanical;
2. **a new fixture whose function is genuinely unsupported on AsmJIT in frame mode** --
   the rollback guard depends on it. Candidates: an unsupported opcode, or a value type
   the body lowering still rejects. This is a *design* question: what is still
   impossible on AsmJIT once the frame form is available?

That second item is the real content of option 1's step 2, and it is smaller and
sharper than "31 fixtures": **one fixture, one test, three lines.**

## 13. Option 1 landed: AsmJIT's frame form is a decision

```
All tests passed (21867 assertions in 777 test cases)
```

Three call sites now read the same predicate:

```cpp
TypeTranslator::moduleNeedsFrame(module, WASM::frameCallAbiOptIn())
```

-- the signature loop in `translateTypes`, the `FunctionCompiler` constructor local and
its `frameMode` member. A module whose signatures fit the register form keeps it; one
that does not switches wholly, which is option 1 as chosen.

### The two blockers, and how each was cleared

**1. The `v128` gap pin** became a positive test: a `v128` parameter now yields
`frameCallAbi() == true` and an exportable function, instead of a refusal. The pin
did its job -- it failed the moment the gap closed, which is why it was written.

**2. The rollback safety test needed a durable "unsupported" probe.** Its old fixture
used a `v128` signature, which the frame form now accepts. Replaced with a
**multi-value block** (`block (result i32 i32)`), which AsmJIT's body lowering refuses
whatever the calling form is:

> The probe must be something the *body* refuses, not something the *boundary* used to
> refuse. Boundary refusals keep being retired by this campaign; body refusals do not.

That is the general lesson for every "unsupported" fixture in the tree: anchor it to a
limitation the work is not currently removing, or the test will flip the moment a
feature lands -- and a safety test that flips is worse than no test, because it looks
like coverage.

### What this changes

* AsmJIT no longer depends on a process-wide switch for a per-signature property;
* the 31-fixture migration feared in section 6 did **not** happen: the suite went from
  21866 to 21867 assertions with **one** test converted and **one** fixture replaced;
* the opt-in remains meaningful for LibJIT, where it is the stated policy, and as an
  escape hatch for AsmJIT (step 3 can now remove it there).

## 14. A step 3 attempted and deferred: the opt-in still has a job on AsmJIT

Dropping the opt-in from AsmJIT's three sites (passing `false` to `moduleNeedsFrame`)
produced, immediately:

| Failure | Meaning |
|---|---|
| "AsmJIT refuses a frame-mode module that calls an import" (P0b) | the fixture has scalar signatures, so nothing needs the frame now, so the module never enters frame mode and the refusal is unreachable -- **the test stops testing the guard** |
| "GC reclamation: a cycle still referenced from outside survives the trial" | collateral: the P0b failure leaves a half-instantiated module, and a later GC test faults |
| segfault after the GC failure | same cascade |

Reverted; suite back to 21867 assertions in 777 cases.

### Why the opt-in is not redundant (yet)

It is the only lever that can put a **narrow** module into frame mode, and two things
depend on that:

1. **P0b's guard is only reachable in frame mode** -- a module with imports and scalar
   signatures is refused *because* it is frame-mode. Without the lever the guard is
   dead code with a passing test, which is the exact failure mode section 13 warns
   about ("a safety test that flips looks like coverage").
2. **The frame-mode tests for imports, multi-result and vectors all set it**, and one
   of them (imports) needs it because its shape is deliberately narrow.

### What step 3 actually requires

Either

* make the P0b fixture **wide** (add a `v128` parameter next to the import), so the
  guard is reachable through the measurement instead of the lever; or
* accept that the opt-in is part of AsmJIT's contract as a *testing and escape* lever,
  and strike step 3 from the plan.

The first is a two-line fixture change and keeps the option open; the second is
honest if nobody wants per-process control. **Neither is urgent** -- the reported
defect (a per-signature property decided by a global switch) is already fixed by
option 1.

## 15. Ruling: no partial modules, on any backend

> **A "partial module" is garbage and useless**, because it has functions that cannot
> be called -- and it is strongly implied that if a function is exported, it is going
> to be called.

All three backends now fail the **module**:

| Backend | Before | Now |
|---|---|---|
| **SLJIT** | recorded the reason, left `fnPtr` null, module instantiated | **throws**, naming the function and the reason |
| **AsmJIT** | threw | threw (unchanged, ruled earlier) |
| **LibJIT** | threw | threw (unchanged) |

The SLJIT change is one branch:

```cpp
if (!dispatcher.compile(entry))
    throw std::runtime_error("the SLJIT backend could not compile function "
            + std::to_string(functionIndex) + ": " + dispatcher.unsupportedReason());
```

### Two tests encoded the old policy, and both were rewritten

| Test | Old expectation | New expectation |
|---|---|---|
| "sljit: bulk memory on a memory64 is refused per function" | module instantiates; `copy->fnPtr == nullptr` | the module **throws** -- renamed to "refuses the whole module" |
| my `[staging][sljit][abi][reason]` | reason recorded on a live instance | the instantiation **throws** |

Both tests were *correct* under the old policy, which is why they are the right place to
look for policy drift: a behaviour change shows up as a small number of tests encoding
the previous rule.

### What becomes vestigial, and what does not

`ModuleInstance::unsupportedReason(name)` (EXPORT_SIGNATURE §43) no longer has a
producer: no backend instantiates a module with an uncallable function, so there is
never a live instance to ask. It stays for now -- removing it would touch a header and
its test for no behavioural gain -- but it is **documented here as unused**, so the next
reader does not assume it carries meaning.

**The reason text did not go to waste**: it is now part of the exception message, which
is strictly better than a query an embedder has to know to make.

Suite 21863 assertions in 777 cases, green.

## 16. Item 4, first probe: exceptions are SILENT on LibJIT -- a defect, not a gap

The probe (`exceptions_probe.wat`) declares a tag and executes `throw`, then records what
each backend does. Silence was the open question; this is the answer:

| Backend | Result | Verdict |
|---|---|---|
| **AsmJIT** | refused at instantiation: *"opcode dispatch is not implemented: dispatchThrow"* | **clean refusal** |
| **SLJIT** | refused at instantiation: *"the SLJIT backend does not lower dispatchThrow"* | **clean refusal** |
| **LibJIT** | executes and **returns normally** | **DEFECT** |

### Why LibJIT's behaviour is the worst of the three

A `throw` that returns a value is not a missing feature -- it is a **wrong answer**. Code
that relies on the trap never runs its handler, and nothing reports a problem: the value
0 comes back as though the function had succeeded. Under the no-partial-modules ruling
this is also unrepresentable: the module instantiates, exports a function, and that
function silently lies.

### What the test does about it

The two refusals are asserted (`REQUIRE_THROWS`). LibJIT's defect is **pinned, not
tolerated**: the test asserts the current wrong value with a `KNOWN DEFECT` comment, so
the day it becomes a refusal or a real trap the assertion fails and the test is
rewritten. A pinned defect cannot be forgotten, which is the property a skip or a
comment would not have.

### The fix, when it is scheduled

LibJIT must either lower `throw` into the pending-trap mechanism the other paths use
(the project's traps are a *pending status*, not a non-local jump -- `WasmTrap.hpp`), or
refuse the module. **Refusing is the smaller change and matches the other two
backends**; lowering is the feature. The same choice was made for multi-result and for
calls, and in both cases the refusal came first.

Suite 21870 assertions in 778 cases, green.

## 17. The capability matrix, measured (started)

Every row below is a **probe that runs on all three backends**, not a reading of refusal
strings. The rule for every row: a backend either does the thing or refuses the
**module** with a reason -- never a silent wrong answer.

| Feature | SLJIT | AsmJIT | LibJIT | Probe |
|---|---|---|---|---|
| `v128` parameters / results | ✅ | ✅ | ✅ (aggregate) | step 5 gates |
| multi-result | ✅ frame | ✅ frame | ✅ register (MV5) | `[multi]` gates |
| compiled-to-compiled calls | ✅ | ✅ | ⛔ refuses in frame mode | step 3b / 5d / P0a |
| imports in frame mode | n/a | ⛔ refuses | ⛔ refuses | P0b / P0a |
| **exceptions (`throw`)** | ⛔ refuses | ⛔ refuses | ❌ **silent wrong answer** | `[exceptions]` |
| **`externref`** | ✅ round-trips | ✅ round-trips | ✅ round-trips | `[refs]` |
| `funcref` / GC refs / structs / arrays | **unprobed** | **unprobed** | **unprobed** | — |
| tail calls in frame mode | ⛔ refuses | **unprobed** | **unprobed** | — |
| atomics / threads | **unprobed** | **unprobed** | **unprobed** | — |
| memory64 | ✅ (bulk incl.) | ✅ (bulk incl.) | ✅ (bulk incl.) | `[m64]` |

### Correction this matrix forced

Section 44 of `EXPORT_SIGNATURE.md` claimed reference types were **LibJIT-only**. That
was wrong, and the probe says so: **all three backends round-trip an `externref`** --
the refusal strings it was based on (*"unsupported Wasm value type"*) evidently apply to
other types, not to references generally.

> A refusal string tells you what a backend *will not* do. Only a probe tells you what
> it *does*. The survey was a hypothesis list; this table is evidence.

### Why the assertions are shaped this way

Each probe asserts the **property** it can prove:

* exceptions: the two refusals are `REQUIRE_THROWS`; LibJIT's defect is **pinned**
  (asserting the current wrong value) so it cannot be quietly forgotten;
* `externref`: if a backend instantiates, the reference must **round-trip identically**
  -- a backend that returns a different pointer is a defect, not a refusal;
* every probe prints its outcome with an `EXC`/`REF` prefix, so a red run names the
  backend without a debugger.

Suite 21876 assertions in 779 cases, green.

## 18. Probe hygiene: a fabricated pointer is not a neutral test value

The `externref` probe's first version passed **`0x1234`** as the reference and still
reported a clean round-trip on all three backends. It also broke an unrelated test far
later in the suite:

```
"AssemblyScript arrays and cached host handles cross the native-import boundary"
Test/main.cpp:12066 FAILED
```

The fabricated pointer was retained in host-handle bookkeeping, and the next test that
used that machinery found it. The probe still works with **`nullptr`**, which is a legal
`externref` and a valid round-trip property.

### The two lessons, one of which is about my own process

1. **Test data must be inert.** A pointer-shaped value handed to a runtime that tracks
   pointers is not neutral: it is an input to a global table. `nullptr` proves the same
   property without entering any structure.
2. **I committed with a red suite.** The probe passed in isolation, and I trusted that
   instead of running the full suite before committing -- the exact shortcut this
   campaign has been avoiding all along. The check is one command and it caught the
   problem one commit late.

> **A filtered green run is not a green tree.** `[refs]` passing says the probe works;
> only the whole suite says the probe is safe.

### Matrix state after the correction

`externref` remains **✅ on all three backends**, measured with a null reference, and the
probe now leaves no trace in shared state. Suite 21876 assertions in 779 cases, green.

## 19. Probes 3 and 4: `funcref` and tail calls both work everywhere

```
PROBE funcref_probe  on libjit: keep=1 ref=1     PROBE tailcall_probe on libjit: tail(7)=7
PROBE funcref_probe  on asmjit: keep=1 ref=1     PROBE tailcall_probe on asmjit: tail(7)=7
PROBE funcref_probe  on sljit:  keep=1 ref=1     PROBE tailcall_probe on sljit:  tail(7)=7
```

Zero refusals across six combinations. Two matrix rows filled, and both were cheaper than
feared:

| Feature | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|
| **`funcref` parameters and results** | ✅ | ✅ | ✅ |
| **tail calls (`return_call`)** | ✅ (register form) | ✅ | ✅ |

### The distinction the tail-call row needs

The probe's signature is `(i32) -> i32`, so **every backend keeps the register form** and
the tail call never touches the frame trampoline. SLJIT's documented refusal ("the
trampoline carries no frame pointer") therefore does not appear here.

So the correct reading is: **tail calls work in the register form on all three; the
unprobed question is a tail call from a FRAME-FORM function.** That is a different
fixture -- three `v128` parameters plus a `return_call` -- and it is now the only known
hole in this row.

### A fixture-validation lesson, worth one line

The first `funcref` fixture failed to validate: *"undeclared function reference 0"*.
`ref.func` requires the function to appear in a declarative element segment. That is a
test-authoring rule, not a backend fact -- and it is worth recording because the failure
looked like a backend refusal for a moment.

Suite 21891 assertions in 780 cases, green.

## 20. The probes did not break the suite: order-dependent flakes were already there

After probes 3 and 4 the full suite failed intermittently, in tests *unrelated to them*:

```
stack-switching resume_throw dispatches a handler clause      Test/main.cpp:13391
simd: float arithmetic and comparisons run through the JIT    Test/main.cpp:8085
```

The evidence that they are pre-existing, not caused by the probes:

| Run | Result |
|---|---|
| `~[tail]` -- probes **excluded** | pass / **2 failed** / pass |
| full suite, `--rng-seed 12345` | **pass** / **pass** |

* the failures occur with the new probes **excluded**;
* they disappear with a fixed seed, so they are **order-dependent**, not test-specific;
* the failing test differs run to run, and the assertion total differs too (21869 vs
  21890), which is what a test aborting early looks like.

Adding test cases changed Catch2's random order, which is enough to expose them. **The
probes are not implicated.**

### Recommendation, stated as a next step rather than done

Pin the seed for CI runs (`--rng-seed <constant>`, or a default in the test main) so the
suite is reproducible, and **then** bisect the stack-switching interaction with a known
order. Until the seed is pinned, "the suite is green" is a statement about *one* order,
and this campaign has already learned what a filtered green run is worth (section 18).

### Correction to section 19

Section 19 recorded "Suite 21891 assertions in 780 cases, green" at the moment of
commit; the run it quoted was one of the passing orders. The honest statement is:
**21891 assertions in 780 cases under a fixed seed; intermittent, pre-existing
order-dependent failures otherwise.**

## 21. The matrix, consolidated (supersedes section 17)

**Read with section 20 in mind**: the suite is deterministic under a fixed seed, so every
row below is reproducible.

| Feature | SLJIT | AsmJIT | LibJIT | Probe |
|---|---|---|---|---|
| `v128` parameters / results | ✅ | ✅ | ✅ aggregate | step-5 gates |
| multi-result | ✅ frame | ✅ frame | ✅ register (MV5); ⛔ frame | `[multi]` |
| compiled-to-compiled calls | ✅ | ✅ | ⛔ frame mode | 3b / 5d / P0a |
| imports in frame mode | n/a (typed) | ⛔ refused | ⛔ refused | P0b / P0a |
| **exceptions (`throw`)** | ⛔ refused | ⛔ refused | ❌ **silent wrong answer** | `[exceptions]` |
| **`externref`** | ✅ | ✅ | ✅ | `[refs]` |
| **`funcref`** | ✅ | ✅ | ✅ | `[refs][tail]` |
| **tail calls, register form** | ✅ | ✅ | ✅ | `[refs][tail]` |
| **tail calls, FRAME form** | ⛔ refused | unprobed | unprobed | next probe |
| GC refs, structs, arrays | unprobed | unprobed | unprobed | — |
| atomics / threads | unprobed | unprobed | unprobed | — |
| memory64 | ✅ (bulk incl.) | ✅ (bulk incl.) | ✅ (bulk incl.) | `[m64]` |

### How to read the symbols

* ✅ -- the probe measured it working, with the probe's fixture shape quoted in the last
  column;
* ⛔ -- **refused at instantiation with a reason**. Under the no-partial-modules ruling
  this is a *correct* answer: the module is rejected, not half-built;
* ❌ -- **wrong answer**: the backend accepted the module and did the wrong thing. There
  is exactly one, and it is pinned by a test so it cannot be forgotten;
* *unprobed* -- nobody has run a fixture. **Not** the same as unsupported, and this note
  keeps the two words apart deliberately: the differences between "we do not know" and
  "it refuses" is the whole reason this matrix exists.

### The one ❌ is the whole point of the exercise

Every other cell is either working or honestly refused. LibJIT's silent `throw` is the
only place where an embedder is told nothing and gets the wrong answer -- and finding it
required a probe, because the tree was *silent* rather than wrong-sounding.

## 22. Probe 5: the frame-form tail call -- refused, correctly, by both backends that have it

```
TAILFRAME libjit: register form -- probe does not apply
TAILFRAME asmjit: REFUSED: AsmJit currently supports only i32 tail-call parameters
TAILFRAME sljit:  REFUSED: the SLJIT backend does not lower return_call
                           (a frame-form module with internal calls)
```

Three honest answers, and **no silent wrong behaviour**. The matrix row is filled:

| Feature | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|
| tail call from a frame-form function | ⛔ refused | ⛔ refused | **n/a** -- register form handles the fixture |

### Why LibJIT is "n/a" rather than a pass

The same fixture gives LibJIT **three `v128` parameters**, which its register form
already delivers as aggregates -- so it never enters frame mode and the question does not
arise. Calling that a ✓ would be wrong: it would say "LibJIT supports frame-form tail
calls" when the honest statement is "LibJIT never needed the frame here".

That distinction is worth keeping in the matrix vocabulary: **n/a is not a pass**, and a
row that silently counts it as one flatters the backend.

### What closing this row would take

* **SLJIT**: the trampoline must carry the frame pointer -- the thunk already takes five
  arguments, so a sixth is the least invasive shape (PRODUCTION_GAPS §2.1);
* **AsmJIT**: the tail-call path is scalar-only (`i32` parameters); a frame-form tail
  call needs the same packing the direct call path already does in `frameMode`;
* **LibJIT**: nothing, unless its frame form ever becomes the default.

Suite 21894 assertions in 781 cases under `--rng-seed 12345`, green.

## 23. The full ledger: defects and honest refusals (probed on all three backends)

This is the list the probe sweep was for. **Nothing is fixed here** -- the policy in force
is *record, document, keep probing*.

### Defects (a wrong answer -- the backend accepted the module and did the wrong thing)

| # | Defect | Backend | Evidence | Status |
|---|---|---|---|---|
| **D-1** | a `throw` executes and the function **returns normally** -- the trap silently becomes a value | **LibJIT** | `[exceptions]` probe, pinned: the test asserts the wrong value with a `KNOWN DEFECT` comment | open, pinned |

**One defect.** Every other cell in the matrix either works or refuses.

### Honest refusals (temporary by definition -- each is a scheduled limitation)

| # | Refused shape | SLJIT | AsmJIT | LibJIT | Message |
|---|---|---|---|---|---|
| R-1 | `throw` | ⛔ | ⛔ | (D-1) | *opcode dispatch is not implemented: dispatchThrow* |
| R-2 | atomics (`i32.atomic.rmw.add`) | ⛔ | ⛔ | ✅ | *does not lower dispatchI32AtomicRmwAdd* |
| R-3 | tail call from a frame-form function | ⛔ | ⛔ | n/a | *supports only i32 tail-call parameters* (AsmJIT) |
| R-5 | calls inside a frame-mode body | ✅ works | ✅ works | ⛔ | *a call inside a frame-mode function is not implemented* |
| R-6 | imports in a frame-mode module | n/a | ⛔ | ⛔ | *calls into imports are not implemented* |
| R-7 | multi-result in frame mode | ✅ works | ✅ works | ⛔ | *multiple results are not implemented* |
| R-8 | unsupported functions | ⛔ module | ⛔ module | ⛔ module | module-level throw (policy, not a limitation) |

### Unprobed (no fixture has run)

| Area | Why it is open |
|---|---|
| exception **handlers** (`try_table`, `catch`) | only `throw` was probed; SLJIT and AsmJIT refuse the throw, so the handler path is untested even where the tag is parsed |
| GC arrays **beyond `array.new`** (`array.get/set/fill/copy`) | AsmJIT's own refusals name packed-element gaps there |
| `funcref` in **tables** / `call_indirect` through a reference | probes crossed references as *values* only |
| threads (shared memory beyond a single atomic RMW) | one instruction probed |
| reference types **as struct/array fields** | constructors probed, contents not |

### Reading the ledger

* **D-1 is the only defect**, and the only cell where an embedder gets a wrong answer
  without being told;
* **R-1 to R-4 are feature gaps** -- real work, honestly refused;
* **R-5 to R-7 are *policy* choices** made in this campaign (LibJIT's frame form is
  opt-in and deliberately partial), not defects;
* **R-8 is the ruling**, not a limitation;
* the unprobed list is where the next defect could still be hiding -- and the reason to
  keep probing rather than declare victory.

## 24. Ledger, second revision (after probes 13-16)

### Defects

| # | Defect | Backend | Confidence |
|---|---|---|---|
| **D-1** | an **uncaught** `throw` returns normally instead of trapping (caught throws work correctly -- probe 13) | LibJIT | high |
| **D-2** | `array.set` / `array.fill` **abort the process** (assertion in `popValueRaw`) | LibJIT | high (needs a minimal repro) |
| **D-3** | an `externref` passed through a struct field comes back **non-null** for a null input | SLJIT | high (pinned) |

**Two backends, three defects.** AsmJIT has none.

### Rows completed since the first ledger

| Feature | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|
| exception handlers (`try_table` + catch) | ⛔ | ⛔ | **✅ works** |
| GC array ops (`set`, `fill`) | ? | ? | **abort (D-2)** |
| `funcref` in a table + `call_indirect` | ✅ | ✅ | ✅ |
| `externref` in a struct field | **❌ D-3** | ✅ | ✅ |
| bulk memory on memory32 | ✅ | ✅ | ✅ |
| multi-memory | ✅ | ✅ | ✅ |

### Retracted

**Q-1** (`array.set` operand order) is **not a defect**: the tree has passing spec tests for
`array`, `array_fill` and `array_new_elem`, and my fixture had two invalid versions. The
probe measured itself, not the feature.

### Still unprobed

| Area | Note |
|---|---|
| threads beyond a single atomic RMW | one instruction probed; shared memory itself works on LibJIT |
| `array.get/set/fill` **on AsmJIT and SLJIT** | Q-1 left this row unresolved: the fixture needs fixing before the row can be filled |
| GC arrays of **reference** elements | values probed, reference elements not |
| exception **handler payloads** across instances | single-module only so far |

## 25. Defect round: the ledger is closed (2026-11-07)

**Snapshot (2026-11-07, `backendsMh33`):** suite **22012 assertions in 791 cases** under
`--rng-seed 12345`, green; `[headerdoc]`, `[docs]`, `doxygen` (0 warnings) and `abi-check`
all pass. This section is the measured resolution of the three defects recorded in
sections 23-24, and it supersedes them.

### What each defect turned out to be

| # | Recorded | Measured now | Resolution |
|---|---|---|---|
| **D-1** | LibJIT: an uncaught `throw` returns normally instead of trapping | **Not a backend defect.** An uncaught throw is defined to return normally with a pending exception (`docs/ABI.md`, `docs/proposals/EXCEPTIONS.md` EXD1), and `Test/main.cpp` already asserted it. The probe asserted a trap instead. | probe rewritten to assert the documented property; no backend change |
| **D-2** | LibJIT: `array.set` / `array.fill` abort the process | **Not reproducible on LibJIT; a real wrong answer on AsmJIT and SLJIT.** Both popped `array.fill`'s value as the offset. | both lowerings fixed; the parity fixture corrected; LibJIT added to the parity test; the probe strengthened to observe the writes |
| **D-3** | SLJIT: an `externref` through a struct field is non-null for a null input | **Real, exactly as recorded.** `struct.get` computed the field width and then loaded/stored 4 bytes regardless. | the load op and the slot width follow the field type |

### D-1: the probe measured its own premise

`throw` with no handler in the function sets the pending-exception slot and returns zeroes
(EXD1); it does not set the *trap* slot. The probe called `do_throw` and asserted "must
never return normally", so the correct behaviour looked like a defect. The rewritten probe
clears the slot, calls, checks the zero result, takes the payload and compares its tag id
with `ModuleInstance::tagIdForIndex(0)`. LibJIT's handler and cross-instance paths were
already measured by probes 13 and 21 and are unchanged.

### D-2: the operand order, and the fixture that hid it

`array.fill` is `[array, offset, value, count]` — the validator in `WasmValidator.cpp`
pops in that order, and upstream `array_fill.wast` invokes it as `(offset, value, count)`.
AsmJIT popped the second-from-top slot as the offset (it is the value) and SLJIT named the
depths the same way. Their shared fixture `gc_array_fill.wat` was written value-first, so
the two wrong backends agreed with each other and both parity tests were green. LibJIT kept
the spec order (the value-first fixture is an out-of-bounds fill under that reading), so the
two contracts were never compared.

The fix is two pop orders and one fixture. The parity test now instantiates AsmJIT, SLJIT
and **LibJIT** from the corrected fixture, so a divergence fails instead of producing
mutual agreement. The strengthened probe (`set_get`, `fill_get`, `inline`) returns the value
it read back; the old probe returned a constant and could not observe the mutation.

The `popValueRaw` abort recorded as the LibJIT symptom did not reproduce with the committed
fixture on a build with assertions active (`-O2`, no `NDEBUG`). It is recorded here as
unconfirmed rather than fixed: the claim that led to it was a fixture that did not observe
its own stores.

### D-3: the width follows the field type

`SljitOpcodeDispatcher::dispatchStructGet` now selects `SLJIT_MOV32` for `i32`/`f32` and
`SLJIT_MOV` otherwise, stores the slot with the same width, and refuses packed and `v128`
fields explicitly (the packed forms belong to `struct.get_s`/`struct.get_u`). The
`REFFIELD` probe requires `nullptr` on every backend instead of pinning SLJIT's old
non-null answer.

### Corrected capability rows

| Feature | SLJIT | AsmJIT | LibJIT | Probe |
|---|---|---|---|---|
| exceptions (`throw`, `try_table`) | ⛔ refuses | ⛔ refuses | ✅ pending-exception protocol | `[exceptions]` |
| GC array `set` / `fill` | ✅ | ✅ | ✅ | `[gc]` |
| `externref` in a struct field | ✅ | ✅ | ✅ | `[tables][gc]` |
| `array.get` (including a reference element array) | ✅ | ✅ | ✅ | `[gc]` |

### The two lessons

1. **Agreement between two backends is not a spec check.** A parity test can only prove
   they agree; when both encode the same misreading it proves the wrong thing. Compare the
   operand order with the validator and the upstream `.wast`, and put the third backend in
   the test.
2. **A probe must observe the property.** Executing a store and returning a constant
   cannot distinguish a correct lowering from a no-op. Return the value that was read back.

## 26. Capability gaps closed, and the findings they exposed (2026-11-07)

**Snapshot (2026-11-07, `backendsMh33`):** suite **22058 assertions in 792 cases**
under `--rng-seed 12345`, green; `[headerdoc]`, `[docs]`, `doxygen` (0 warnings) and
`abi-check` all pass. This section supersedes the remaining rows of sections 23-25.

### Closed

| Capability | SLJIT | AsmJIT | LibJIT | Evidence |
|---|---|---|---|---|
| `throw`, `throw_ref`, `try_table`, `catch`, `catch_all` | ✅ | ✅ | ✅ | `[exceptions]`: local catch 42, cross-instance 7, uncaught pending set |
| memory atomics: load/store/RMW/cmpxchg, fence, pause, notify, wait | ✅ | ✅ | ✅ | `[atomics]`: `bump=0,1`; `i32=9 i64=10 i8=0 i16=0` |
| frame-form `return_call` / `return_call_indirect` | ✅ | ✅ | ✅ (register form) | `[tail]`: `tail_wide=1` |

The exception lowering is one protocol on all three backends: a Store-owned object
with a canonical payload layout, a per-execution-state pending slot, a call-site
check, and runtime clause dispatch for `try_table`. The atomics use the shared
`wasm_atomic_*` helper ABI (bounds-checked address in generated code, alignment and
memory order in the helper), so no target-specific atomic instruction is required.
Frame-form tail calls are call + frame return: the values are correct and the
staging-frame ABI is honoured end to end. (Later passes added driver-backed frame
tails on SLJIT/AsmJIT; LibJIT's opted-in frame tails remain call+return.)

### Still refused (historical, superseded by section 27)

| Gap | SLJIT | AsmJIT | LibJIT | Reason |
|---|---|---|---|---|
| `catch_ref` / `catch_all_ref`; a catch payload wider than one value | n/a (supported) | ⛔ | n/a (supported) | a structured block on AsmJIT carries at most one result |
| shared-everything atomics on reference/table slots | ⛔ | ⛔ | ✅ | numeric global/struct/array slots are done; shared refs need shared composites and tables need table.get/set |

Both rows were closed in section 27; the table is kept as history. Production
readiness is a different question, recorded in section 29.

### Findings

**V-1 -- the validator accepts a block that leaves extra stack values.**
`exc_handler_probe` and `tag_consumer` declared `try_table (result i32)` and then
pushed another value before the enclosing `end`. `wasm-tools validate` rejects both
("values remaining on stack at end of block"); this engine's validator accepted them,
and LibJIT/SLJIT executed the catch path, which hid the invalidity. Both fixtures are
corrected. The validator's block-end stack check is a core gap, independent of any
backend, and is **open**.

**P-1 -- SLJIT's dead function end ignored the frame form (fixed).** After a tail
call the function-wide `end` is unreachable; the epilogue then emitted a typed return
for a `void(ctx, frame)` signature. SLJIT asserts on that mismatch and aborted the
process. The dead path now returns void in frame mode.

**P-2 -- AsmJIT's frame-mode float return was unreachable/wrong (fixed).** The
function-end path tested `isFloatKind` before `frameMode`, so a float result was
returned in an Xmm from a void signature. Frame branches now come first; one
`emitFrameReturnValue` serves the normal end, an explicit `return` and `return_call`.

**P-3 -- the measuring pass is part of the lowering (fixed).** The first SLJIT atomic
helpers pushed their result only in the emit pass; the measuring pass then underflowed
on the next instruction. Every new lowering must update the compile-time stack model
in both passes.

## 27. V-1 fixed, multi-value catches and numeric shared atomics closed (2026-11-07)

**Snapshot (2026-11-07, `backendsMh33`):** suite **22442 assertions in 821
cases** under `--rng-seed 12345`, green; `[headerdoc]`, `[docs]`, `doxygen`
(0 warnings) and `abi-check` all pass. This section supersedes section 26.

### Closed

| Capability | SLJIT | AsmJIT | LibJIT | Evidence |
|---|---|---|---|---|
| `catch_ref` / `catch_all_ref` and multi-value catch payloads | OK | OK | OK | `catch2=22 catch2i32=33 catchref=33` |
| multi-value structured blocks | OK | OK | OK | `block2=33 direct=33` |
| direct cross-function `return_call` (mutual) | OK | OK | OK | `TAILMUTUAL` 1,000,000 links, `run()=1` |
| indirect cross-function `return_call_indirect` | OK | OK | OK | `TAILINDIRECT` 1,000,000 links, `run()=1` |
| frame-form cross-function `return_call` (scalar) | OK | OK | OK | `TAILFRAME` 1,000,000 links, `run()=1` |
| frame-form cross-function `return_call` (v128) | n/a | OK | OK | `TAILV128FRAME` 1,000,000 links, `run()=1`; LibJIT native trampoline excludes v128 |
| `call_ref` / `return_call_ref` (register form) | OK | OK | OK | `TAILREF` 1,000,000 links, `even0=1 run()=1` |
| frame-form `call_ref` / `return_call_ref` | OK | OK | OK | `TAILREFFRAME` 1,000,000 links, `run()=1` |
| v128 frame-form `return_call_ref` | n/a | OK | OK | `TAILV128REFFRAME` 1,000,000 links, `run()=1`; LibJIT native trampoline excludes v128 |
| `table.get` / `table.set` (externref/funcref, OOB trap) | OK | OK | OK | `TABLEACCESS` ext=1 fun=0 |
| shared reference atomics (struct/array) | OK | OK | OK | `SHAREDREF` run=1 array=1 |
| shared table atomics (anyref/eqref) | OK | OK | OK | `TABLEATOMIC` run=1 |
| global reference atomics (eqref) | OK | OK | OK | `GLOBALREF` run=1 |
| v128 atomic slots rejected at validation | OK | OK | OK | `V128ATOMIC` rejected (global/struct/array) |
| numeric shared-everything atomics (global/struct/array) | OK | OK | OK | `global=5 struct32=7 struct64=9 array=11` |
| V-1: block-end stack check after a `try_table` | n/a | n/a | n/a | `validator: a catch does not disable block-end stack checks` |

### V-1: the taint, and what it hid

`Validator::dispatchTryTable` set `typeTainted = true` for any `try_table` with
catches. That flag makes `popAny`/`pushValue` skip the operand stack for the
rest of the module, so a module could leave arbitrary extra values at a block
end and still validate. The catch payload is already checked against the target
label at the `try_table` instruction, so the taint was removed. The regression
fixture `invalid_catch_stack.wat` is rejected by the validator and by
`wasm-tools validate`.

The taint had hidden four invalid hand-written fixtures (`exceptions.wat`,
`native_import_exceptions.wat`, `cont_throw.wat`, `cont_throw_handler.wat`); all
are corrected. Removing it also exposed two real validator defects, both fixed:

* **`throw_ref`** popped the bare `ExnRef` opcode, but the stack holds a
  `(ref null exn)` (`Ref`/`RefNull` with the exn heap type). `refSubtype` now
  checks the reference form, and `spec: throw_ref` is 13/0/2 again.
* **`TypeTranslator::moduleNeedsFrame`** scanned the whole type section, so a
  synthetic multi-result *block* type switched an AsmJIT module to the frame
  ABI. It now scans only import and internal function signatures.

### Multi-value blocks and catches

AsmJIT's control frames now hold a vector of result entries, so a block can
carry N results (scalar registers or a 16-byte vector buffer); `br`, `br_if`, `br_table`, `else` and `end` transfer all of
them. `emitCatchBranch` writes each payload field (plus the `exnref` for
`catch_ref`) into the target's result registers. The old single-result refusals
are gone. `dispatchUnreachable` now returns a correctly sized zero for i64 and
uses the frame ABI when the signature is frame-mode.

### Numeric shared-everything atomics

Globals, struct fields and array elements lower `atomic.get/set/rmw/*` through
the same `wasm_atomic_*` helpers as linear memory, including packed fields and
the 64-bit forms. Reference slots use `wasm_atomic_ref_*`; shared-table atomics
use the core table helpers. `v128` atomic slots are **not** allowed by the
proposal; the validator rejects them instead of leaving a backend refusal.

### Still open

| Gap | Backends | Reason |
|---|---|---|
| *(none in this curated capability ledger)* | -- | No implemented-feature row remains. **Production readiness is a separate question**; see section 29. |

`v128` atomic slots are not in this table: the proposal does not allow them and
the validator rejects them (`V128ATOMIC`).

## 28. LibJIT frame calls, imports and multi-result -- implemented (2026-11-07)

**Policy unchanged:** the register form is LibJIT's default; the frame form is used
only when `WASM::setFrameCallAbiOptIn(true)` is requested.

Once opted in, the frame contract is complete:

* the signature is canonical `void fn(VMContext*, void* frame)`;
* the prologue reads parameters at `WASM::frameValueBytes` offsets and the epilogue
  writes one or N results into the caller's result block;
* `emitFrameInvocation` allocates a local call frame, stages the arguments, calls
  `fnPtr(ctx, frame)`, checks the pending exception slot, and pushes every result;
* direct, indirect and `call_ref` calls use the same lowering, and a native import
  receives the same `(ctx, frame)` pair;
* frame tails are a frame call plus a frame return. They are correct but grow the
  host stack; a frame-aware trampoline is the remaining constant-stack work.

**Evidence (all default-off pins still pass):** frame call 6, native import 42,
multi-result 7/9, 100-link frame tail 1, multi-result `call_ref` 11. The safety probe
uses a rejected artifact and confirms the runtime remains usable.

**Suite:** 22442 assertions in 821 cases, `--rng-seed 12345`, green; `[headerdoc]`,
`[docs]`, `doxygen` and `abi-check` clean.

## 29. Production-readiness gaps -- SLJIT and AsmJIT (2026-11-07)

**Snapshot (2026-11-08, working tree after `ffbbc7f`):** the detailed note is
[`PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md). The headline is:

| Backend | Verdict | Measured evidence |
|---|---|---|
| LibJIT | **Production-ready within its declared envelope** | default suite 22,442 assertions / 821 cases, green; x86-64 Linux only |
| SLJIT | **Not production-ready** | `WASMJIT_BACKEND=sljit`: 821 cases, 720 passed, 101 failed; no crash |
| AsmJIT | **Not production-ready** | `WASMJIT_BACKEND=asmjit`: 821 cases, 677 passed, 144 failed; no crash |

The blockers are not just “unimplemented opcodes”:

* **SLJIT:** `func`, `float_memory64`, `table.fill`, `table.grow`, the integer
  lane families, `br_table` arity, the i64 bit-count family, calls in dead
  code, the `simd_load*_lane` family, `v128` globals and scalar deterministic
  NaN canonicalisation are fixed. The lane fix was a mode-selection bug: the C
  argument count must include the hidden `v128` result out-buffer. Remaining
  failures are stringref/relaxed-SIMD, GC arrays and some transport families.
  The legacy-EH rewrite itself is frontend desugaring, not a backend feature.
* **AsmJIT:** `spec: func`, `spec: block`, `spec: br_table`, `spec: return`,
  `spec: unreachable` and `spec: switch` are green, including frame-form
  function-label branches and the multi-result tail-call fixture. `table.grow`,
  `v128` globals and the integer lane family are implemented. Remaining
  failures include one `table_get` reference-result assertion, several SIMD
  transport/relaxed opcodes and `imports` allocation/trap cases.

Promotion requires the full backend-selected suite to finish green (no abort,
no segfault), the strict negative sweep to pass, the sanitizer run to be recorded,
and the status documents to agree. Until then LibJIT is the only production
backend.

**Reproduce:** `WASMJIT_BACKEND=sljit ./Test/WasmJit --rng-seed 12345` and
`WASMJIT_BACKEND=asmjit ./Test/WasmJit --rng-seed 12345` from
`build/Desktop-Release`.
