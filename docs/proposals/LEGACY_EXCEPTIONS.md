# Legacy Exception Handling (`try`/`catch`) — Compatibility Notes, Encodings, and the Cost of Admission {#prop_legacy_exceptions}

Status: design note / compatibility study (revision 2 — 2026-10-30: `LE-1`…`LE-6`
are **implemented** behind the opt-in switch, and `LE-8` records the one shape the
rewrite refuses; revision 1 was the study). It analyses the **superseded Phase-3
("legacy") exception-handling binary form**, what it cost to accept, and how it
was accepted — and it answers the six questions it was commissioned with in §10.
**Accepting the form is not a support claim**: it is a load-time compatibility
adapter, off by default, and the engine still executes only the final encoding.
It ships no interpreter, no second exception runtime and no engine change to the
backend contract.

Audience: anyone touching the exception front end
(`WasmBase/WasmOpcode.hpp`, `WasmOpcodeDispatcher`, `WasmLegacyEh`,
`WasmValidator`, LibJit's `try_table` lowering), the language-feasibility
charters `S1`/`S2`/`S4` ([`../languages/CHARTERS.md`](../languages/CHARTERS.md)),
and anyone whose C++ or Kotlin/Wasm module loads here and fails with "legacy
exception handling … is not supported".

Follows the same split as [`EXCEPTIONS.md`](EXCEPTIONS.md) / [`GC.md`](GC.md) /
[`SIMD.md`](SIMD.md): §2 what the proposal demands → §3 what it does not demand →
§4 what is in the tree → §5 gaps → §6 where we may deviate → §7 plan → §8 test
survey → §9 references. §10 is an appendix, in the shape `GC.md` §10 and
`STACK_SWITCHING.md` §10 already use, because this note was commissioned with six
specific questions rather than as a proposal to implement.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What does the legacy proposal demand, byte for byte? |
| §3 | What does it *not* demand? |
| §4 | What is in the tree today, and what does it do with a legacy module? |
| §5 | Where do we fall short (`LE1`–`LE9`)? |
| §6 | Where may we play fast and loose (`LED1`–`LED5`)? |
| §7 | Plan (`LE-0`…`LE-6`, plus the open `LE-8`) |
| §8 | Test survey — the fixtures, the `[legacy-eh]` tag and the probe matrix |
| §9 | References |
| §10 | The six commissioned questions: conflicts, safety, rewrite size, usefulness, workarounds, least resistance |

## 2. What the proposal demands

The legacy form is the *third* version of the exception-handling proposal
(adopted 2020-09-15, which removed `exnref`), superseded in October 2023 by the
current proposal, still accepted by browsers, and **deprecated and discouraged**
by its own spec (§9 `R-1`). Two properties matter most:

### 2.1 The parts that are identical to the final proposal

Nothing below needs work, and that is the first half of the answer to §10.1:

| Element | Legacy | Final (and our tree) |
|---|---|---|
| Tag section | code `13`, after memory, before global; entry = `attribute:u8` (`0x00`) + `typeidx` | identical (`Module::processTagSection`, `Tag{attribute, typeIdx}`) |
| Tag import | `external_kind = 4`, `tag_type` immediate | identical (`ImportTag`) |
| Tag export | export kind `Tag` | identical |
| Tag names | name subsection `11` | identical |
| Tag identity | per-module, aliased on import/export, unique per module | identical (`EX-2`) |
| `throw` | **`0x08`** + `tagidx` | **`0x08`** + `tagidx` — same byte, same immediate, same semantics |

### 2.2 The five opcodes that only the legacy form has

| Mnemonic | Byte | Immediate | Meaning |
|---|---|---|---|
| `try` | **`0x06`** | `blocktype` | begins a block whose throws are handled by the `catch` clauses that follow |
| `catch` | **`0x07`** | `tagidx` | ends the current arm and begins a handler for that tag; the tag's parameters are pushed |
| `catch_all` | **`0x19`** | — | begins a handler for anything |
| `rethrow` | **`0x09`** | `reldepth` | rethrows the exception caught by the catch block at that depth |
| `delegate` | **`0x18`** | `reldepth` | **ends** the try and hands its exceptions to the frame at that depth |

### 2.3 The structural difference that makes this a study

The final proposal has **one** structured instruction, `try_table`, whose catch
clauses are *labels*: a throw branches to a label, and the handler body lives
wherever that label is. The legacy proposal has **inline handler bodies** inside
one instruction sequence:

```wat
try (result i32)        ;; one label
  <body>
catch $e                ;; arm 1; payload i32 pushed; must also yield i32
catch_all               ;; arm 2; no payload; must also yield i32
end                     ;; closes the whole try — there is no end per arm
```

Four consequences the rest of this note depends on:

1. **`catch`/`catch_all` are not blocks.** They do not introduce labels, do not
   affect branch depth, and cannot be branched to. A desugaring that turns them
   into blocks therefore has to *recompute* the depths of the branches inside the
   arms (§5 `LE4`).
2. **`delegate` terminates the try** instead of `end`: a delegating try has no
   arms at all. Its `reldepth` names a frame that may be *outside* intervening
   try blocks, so a naive "just fall out of the block" translation is only
   correct when nothing catches in between (§5 `LE3`).
3. **`rethrow` rethrows "the exception caught by the catch block at that depth"**,
   i.e. it reads an implicit *current exception* that the final proposal does not
   have. The only way to express it is to have kept the caught exception, which
   means `catch_ref` plus somewhere to put the reference (§5 `LE2`). It is not a
   corner case: clang emits it for **every C++ cleanup pad** (§4.5 `P9`).
4. **Arms must match the try's result types.** Every arm is a handler whose body
   must produce exactly the `try`'s results; the tag's parameters are extra
   values on top. This is the shape `EXCEPTIONS.md` §2 describes for a `catch`
   clause's label arity, and our validator already checks it for `try_table`
   (`Validator::dispatchTryTable`).

## 3. What it does not demand

Reproduced from the legacy explainer, because §6's deviations lean on it:

- **No unwinding mechanism.** Table-driven unwinding, `setjmp`/`longjmp`, native
  exceptions or CPS are all acceptable — the spec constrains the *observable*
  control flow, not the implementation.
- **No exception object.** A legacy module cannot create, name, store, pass,
  compare or destroy an exception *value*; `catch` yields the tag's parameters
  and nothing else. There is no `exnref` and no `catch_ref`, which is precisely
  why the proposal was superseded ([`EXCEPTIONS.md`](EXCEPTIONS.md) §2).
- **No exception identity beyond the tag.** A catch matches the exact tag thrown;
  there is no subtyping, no payload introspection, no `catch_all_ref`.
- **No host-boundary semantics.** Whether an embedder can observe, fabricate or
  cross an exception is the embedder's business (same stance as the final
  proposal, `EXCEPTIONS.md` §3).
- **No interaction with continuations or `suspend`.** Those are the final
  proposal's business ([`STACK_SWITCHING.md`](STACK_SWITCHING.md)).
- **No performance contract** and **no requirement to keep the deprecated form
  forever**: the spec says the proposal "can be deprecated in future and the use
  of this proposal is discouraged".

`rethrow` is therefore the *only* way to propagate a caught exception, and it can
only rethrow the lexically enclosing catch — a fact §5 `LE2` and §10.3 both turn
on.

## 4. What we have implemented today

### 4.1 The final proposal is complete

[`EXCEPTIONS.md`](EXCEPTIONS.md) §4 records `EX-1`…`EX-7` shipped:
`exn`/`exnref`/`nullexnref` as first-class types, `throw`/`throw_ref`/`try_table`
with all four clause forms, Store-owned `TagId` identity, pending-exception
propagation, `exnref` table round-trips and `assert_exception` in the harness.
[`../READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2 lists the proposal at
**100%**.

### 4.2 The lowering is compile-time static dispatch — and that is the good news

Two code facts decide this study's cost estimate. `dispatchTryTable` pushes a
block frame and records the clauses:

```cpp
// LibJit/LibjitOpcodeDispatcher.cpp
void OpcodeDispatcher::dispatchTryTable(const WASM::BlockType& arg1,
                                        std::vector<WASM::CatchClause>&& arg2)
{
    // The catch clauses are not a runtime construct: every throw site in the
    // dynamic extent dispatches to them statically (EXD2). The instruction
    // itself therefore behaves exactly like a block with a label at its end.
    pushBlockFrame(ControlBlock::TryTable, arg1, std::move(arg2));
}
```

and a `throw` site scans the control stack for the innermost matching clause and
branches to that clause's label with the payload as branch arguments
(`dispatchThrow`, same file). There is **no runtime handler table, no unwind
table and no landing-pad metadata** in the generated code.

Consequence: *any* front-end form that can be expressed as "branch to a label
with the payload on the stack" reuses the entire exception machinery unchanged.
That is exactly what a legacy desugaring is (§10.3).

### 4.3 The front end refuses the legacy opcodes — generically

| Byte | In the tree | Effect on a legacy module |
|---|---|---|
| `0x08` `throw` | implemented (`Opcode::Throw`) | works — the byte is shared with the final proposal |
| `0x06` `try` | **reserved**, no enum arm (`WasmOpcode.hpp`: "0x06-0x07 reserved") | decode fails |
| `0x07` `catch` | **reserved** | decode fails |
| `0x09` `rethrow` | **reserved** ("0x09 reserved") | decode fails |
| `0x18` `delegate` | **reserved** ("0x16-0x18 reserved") | decode fails |
| `0x19` `catch_all` | no arm at all | decode fails |
| `0x1F` `try_table` | implemented | — |
| `0x0A` `throw_ref` | implemented | — |

The failure is `OpcodeDispatcher::dispatchOpcode`'s named arm (see §4.6): it
names the superseded proposal, the instruction, and every remedy. Before `LE-1`
it was the generic `"Invalid or unsupported primary opcode"`, which told a C++
user nothing.

### 4.4 The tag surface is already the legacy surface

`Module::processTagSection` reads `attribute:u8` (currently always `0x00`) then
`typeidx`; `Tag`, `ImportTag` and the export kind are the same shapes the legacy
explainer specifies (§2.1). **A legacy module's tag section, imports, exports and
names need no new decoding at all.**

### 4.5 What the engine does with real modules — measured, twice

Run on 2026-10-30 against this branch's build
(`build/Desktop-Debug/Test/WasmJit`), with wabt 1.0.41, Binaryen 124,
wasm-tools 1.252.0 and clang 23.1.4. The one-off artifacts below live in `/tmp`;
the *permanent* fixtures this note ships are the `Test/wasm_wat/legacy/` set in §8.

| # | Probe | Result |
|---|---|---|
| **P1** | a hand-written legacy module (`try`/`catch $e`/`catch_all`, one tag with an `i32` payload) built with `wat2wasm --enable-exceptions` | built; `wasm-tools print` shows `try` / `catch 0` / `catch_all` |
| **P2** | `wasm-tools validate legacy.wasm` | **fails**: "legacy_exceptions feature required for try instruction (at offset 0x29)". With `--features legacy-exceptions` it **validates** — tooling can inspect the form |
| **P3** | engine: `WasmJit --diagnose=legacy.wasm` | **cannot load**: the `LE-1` message, naming the proposal, `try (0x06)`, the flags and `WASMJIT_LEGACY_EH` |
| **P4** | `wasm-opt --enable-exception-handling --translate-to-exnref legacy.wasm -o exnref.wasm` | **succeeds**; `wasm-tools validate exnref.wasm` passes; the body becomes `block` / `block` / `try_table (catch 0 …) (catch_all …)` + `br` |
| **P5** | engine: `WasmJit --diagnose=exnref.wasm --invoke=f --args=0` | **loads, instantiates, runs**: `result[0] i32 = 7` |
| **P6** | `clang++ --target=wasm32-unknown-unknown -fwasm-exceptions -O1 -c` on a `try`/`catch(E&)` function | emits **legacy** `try` / `catch 0` / `rethrow 0` — the *default* of LLVM 23.1.4 in 2026 |
| **P7** | the same with `-mllvm -wasm-use-legacy-eh=false` | emits `try_table (catch_ref 0 …)` / `throw_ref` |
| **P8** | engine: `--diagnose` on the P6 object and on the P7 object | P6: the `LE-1` named refusal; P7: **loads**, 8 imports (`env.__cpp_exception`, `env.__cxa_*`, …). With `WASMJIT_LEGACY_EH=1` the P6 object loads too (§4.6) |
| **P9** | `clang++ … -fwasm-exceptions` on a function with a **destructor** and no `catch` (`struct G { ~G(); }; void f() { G g; may_throw(); }`) | emits `try` / `catch_all` / **`rethrow 0`** — a cleanup pad. `delegate` does **not** appear |

P4+P5 are the whole answer to "are there workarounds" (§10.5): one Binaryen pass
turns the rejected form into the accepted one, and the engine runs the result.
P6 is the answer to "how useful is it" (§10.4): as of 2026 the current LLVM
**still defaults to the legacy encoding**, so this is not a museum piece. P9
corrects the assumption this study started with, and it changes the priorities:
**`rethrow` — not `delegate` — is on the hot path of ordinary C++**, because
every function with a destructor needs a cleanup pad that rethrows.

### 4.6 What shipped (`LE-1`…`LE-6`)

| Piece | Where | What it does |
|---|---|---|
| **Named refusal** (`LE-1`) | `WasmOpcodeDispatcher.cpp`, `legacyExceptionHandlingMessage()` | The five opcodes decode their immediates and throw a message naming the proposal, the instruction (`try (0x06)`…), the three remedies and this note. `WASM.VALIDATE.RULE` carries it, so `--diagnose` prints it |
| **Opt-in switch** (`LE-6`) | `ParseOptions::legacyExceptionHandling` ([`WasmParseOptions.hpp`](../../WasmBase/WasmParseOptions.hpp)), passed through `Module::fromFile` and consulted in `Module::parseBody` | A parser-side policy, not an environment variable: `module.fromFile(device, WASM::ParseOptions{}.acceptingLegacyExceptionHandling(), store)`. The argument-less overload seeds it from `WASMJIT_LEGACY_EH`, and `module.parserOptions()` reports what an artifact was loaded with. A `Component` carries the same policy into its embedded core modules. Off by default |
| **The rewrite** (`LE-3`, `LE-4`) | `WasmBase/WasmLegacyEh.cpp`, `BodyRewriter` | Runs once, before `validateValueTypes()`/`validate()`. Decodes each body into instruction spans (reusing the engine's own immediate parsing, so operand lengths cannot drift), then emits the §10.3 shape: `block` merge point, payload landing blocks, `try_table` with the same clauses, `br` to the merge, then the arms in binary order. Bodies that do not use the form are left byte-identical |
| **`rethrow`** (`LE-4`) | same | The rethrowing arm's clause becomes `catch_ref`/`catch_all_ref`, the exception reference lands in a synthetic `exnref` local appended to the body's locals, and `rethrow n` becomes `local.get n-th arm; throw_ref`. Payload identity is preserved, and `rethrow 1`+ resolves against the enclosing arms, not against labels |
| **`delegate`** (`LE-5`) | same | A try with no arms becomes a plain `block`: its exceptions propagate. The *skip* case is refused by name (`LE8`) rather than approximated |
| **Branch remap** (`LE-4`, `LE-5`) | `remapDepth()` | `br`, `br_if`, `br_table`, `br_on_null`/`br_on_non_null` and the GC `br_on_cast*` family carry labels that are renumbered, because the rewrite inserts frames between the binary frame and its target |
| **Block types** | `emitBlockTypeForResults()` | One-byte forms where they fit; otherwise a `(param) -> (result…)` type is appended to the module and its index used, which is the only way to spell a multi-value landing block (`(i32, exnref)`) |
| **Fixtures and tests** (`LE-2`) | `Test/wasm_wat/legacy/`, `Test/LegacyEhTests.cpp` (`[legacy-eh]`), `Test/legacy-eh-probe.sh` | Four fixtures (basic shapes, rethrow, branch/delegate shapes, the refusal) plus Binaryen's translated twin, and six test cases |

**What did *not* change**: the exception object and the pending slot
(`WasmExn.hpp`), tag identity and the tag section, `try_table`/`throw`/`throw_ref`
lowering, the validator's clause checking, every backend and the
`WasmBase` ↔ backend contract, and the exported-symbol baseline (`abi-check.sh`:
0 removed, and the five additions are the new `fromFile`/`parseNested` overloads that
carry the policy, which is what `API_ABI_STABILITY.md` `G2` allows and the baseline
now records; the rewrite itself adds no symbol).


## 5. Gaps

Closed rows are struck through with the milestone that closed them; a row that
was closed by *refusing* the construct says so.

| # | Gap | Evidence | Severity |
|---|---|---|---|
| ~~**LE1**~~ | ~~**The front end refuses the legacy form with a generic error.**~~ **Done (LE-1):** `dispatchOpcode` has named arms for all five opcodes, and `legacyExceptionHandlingMessage()` names the proposal, the instruction, the compiler flags, the Binaryen pass and the opt-in switch. **Class A.** | `WasmBase/WasmOpcodeDispatcher.cpp`; `[legacy-eh]` "refused by name" asserts each remedy in the text | Closed |
| ~~**LE2**~~ | ~~**`rethrow` has no expression in the final form without new state.**~~ **Done (LE-4):** the rethrowing arm's clause becomes `catch_ref`/`catch_all_ref`, the reference is kept in a synthetic `exnref` local, and `rethrow` lowers to `local.get; throw_ref`. Payload identity is preserved (`same` = 6, `outer` = 49). **Class A.** | `WasmLegacyEh.cpp` `allocateExnLocals`/`emitRethrow`; `[legacy-eh]` "keeps the exception object across a rethrow" | Closed |
| ~~**LE3**~~ | ~~**`delegate`'s relative depth can skip intermediate catch frames, and the obvious translation is wrong.**~~ **Done for the non-skipping case, refused for the skipping one (LE-5 / `LE8`):** a try with no arms becomes a plain block, and the rewrite checks each frame between the try and its target for an intervening legacy try before accepting it. **Class A.** | `emitLegacyTry`; `[legacy-eh]` "remaps branches and delegate depths" (`d1`, `d2`) and "refused" (`delegate_skip`) | Closed (with `LE8`) |
| ~~**LE4**~~ | ~~**There is no label-depth remap and no module rewriter.**~~ **Done (LE-3):** `BodyRewriter` scans a body into instruction spans, `remapDepth()` renumbers every branch, and `Module::parseBody` runs it once before validation. **Class A.** | `WasmBase/WasmLegacyEh.{hpp,cpp}`; `[legacy-eh]` `b1`–`b4` | Closed |
| ~~**LE5**~~ | ~~**A body rewrite invalidates branch-hint offsets.**~~ **Done (LE-3):** a rewritten body drops its `branchHints`; an untouched body keeps them. | `rewriteLegacyExceptionHandling()` | Closed |
| ~~**LE6**~~ | ~~**No legacy fixtures, and no upstream suite to borrow.**~~ **Done (LE-2):** `Test/wasm_wat/legacy/` holds four `.wat` sources, their committed `.wasm`, and Binaryen's translated twin; `Test/legacy-eh-probe.sh` regenerates them and records `P1`–`P9`. **Class A** (tests only). | `Test/wasm_wat/legacy/`, `Test/legacy-eh-probe.sh`, `[legacy-eh]` | Closed |
| ~~**LE7**~~ | ~~**The default configuration must not accept deprecated input silently.**~~ **Done (LE-6):** `WASMJIT_LEGACY_EH` is the only way in, it is read once per load, and the default path still refuses with `LE-1`'s message. **Class A.** | `ParseOptions::legacyExceptionHandling`, `Module::parseBody`; `[legacy-eh]` first and last tests | Closed |
| **LE8** | **A `delegate` whose target skips an intervening legacy catch is refused.** Honouring the skip needs the *enclosing* try's clauses to be split per region (its arms must not catch this try's throws), which means duplicating arm bodies per region. The rewrite refuses by name instead of miscompiling. Current clang output does not use `delegate` at all (`P9`), so the cost is deferred rather than hidden. **Class A.** | `emitLegacyTry`; `[legacy-eh]` "a delegate that would skip an intervening catch is refused" | Medium (fidelity), Low (frequency) |
| **LE9** | **A body that mixes the superseded form with `try_table` is refused.** The remap would also have to renumber a `try_table`'s catch labels, which the rewrite does not parse. A module that already emits final-form exception code does not need the adapter, so the refusal costs nothing today. **Class A.** | `build()`, `Opcode::TryTable` arm | Low |


Two things that are *not* gaps, recorded so they are not re-opened:

- **The tag surface.** §4.4: section 13, import kind 4, export kind and name
  subsection 11 are byte-identical, and `throw` (`0x08`) is literally the same
  instruction. There is nothing to add.
- **Validation fidelity for catches.** Our validator already stops trusting the
  operand stack after a `try_table` with catches
  (`Validator::dispatchTryTable` sets `typeTainted`), because a catch injects its
  payload into an *outer* label a linear stack model cannot express. A legacy
  front end inherits exactly that limitation; it does not create a new one
  (§6 `LED3`).

## 6. Where we can and should play fast and loose

The freedoms are §3's. Each deviation names the one it uses and the guardrail
that keeps it honest.

| # | Deviation | Why it is safe | What it buys |
|---|---|---|---|
| **LED1** | **Accept the legacy form only behind an opt-in switch; the default stays strict.** ✅ *Adopted as `LE-6`.* | §3 leaves scope to the implementation, and the proposal deprecates itself, so "off unless asked" is the honest default. It also keeps [`../SAFETY.md`](../SAFETY.md)'s "tell artifacts apart before decoding" true: the *default* decoder still refuses a form the spec has retired. | A future embedder can load old modules without making every other embedder accept deprecated input, and no policy has to be reversed later. |
| **LED2** | **Desugar in the front end; never let a backend see a legacy opcode.** ✅ *Adopted as `LE-3`.* | §3 imposes no unwinding mechanism, only observable control flow, and every legacy construct reduces to `try_table` + branches, which the engine already executes (`EX-1`…`EX-7`). | Zero duplication across LibJit/SLJIT/AsmJit, one place to test, and the gap stays **Class A** instead of "Class A in the interface plus Class B in every backend". The alternative — native handler arms — restructures the control-flow model and is **Class E** (§10.3). |
| **LED3** | **Validate the legacy form with the same operand-stack taint the final form already uses.** ✅ *Adopted: the rewrite runs before `validate()`, so the validator sees only final-form code and the taint it already applies.* | The catch payload's stack effect is *already* unmodelled for `try_table` (`typeTainted`); tag indices, label ranges, arm result arity and branch depths are still checked. | No second, better validator has to be built for a deprecated form. |
| **LED4** | **Refuse `delegate` when its target skips an intervening catch (`LE8`); implement the non-skipping case (`LE-5`).** | **Measured:** clang 23 emits `catch_all` + `rethrow` for cleanup pads, not `delegate` (§4.5 `P9`), so the skipped case is rare in current output — and a construct whose skipping semantics is not implemented must not be approximated. `rethrow` is **not** in this bucket: it is on the hot path (`LE2`) and shipped with `LE-3`. | The common case works end-to-end (`d1`, `d2`); the remaining case gets its own milestone, fixture and review. |
| **LED5** | **Do not materialise an `exnref` for a legacy catch unless a `rethrow` needs it.** ✅ *Adopted: only an arm that rethrows gets `catch_ref` and a local.* | §3: a legacy module cannot observe an exception value at all, so retaining one is unobservable. | The fast path (no `rethrow` in the arm) stays a plain `catch` label carrying the payload as branch arguments — the existing lowering, untouched. |

**What we may *not* play loose with**, stated so a later implementer does not:

1. **`delegate`'s target is a depth, not "the nearest handler".** A translation
   that ignores the skip is a wrong program, not a fast program (§5 `LE3`).
2. **A catch arm's result arity is part of validation.** An arm must yield the
   `try`'s results; a payload is *extra* values on top. Our `try_table` check
   already enforces the final form of this rule, and the legacy form must map
   onto it exactly.
3. **`rethrow` must rethrow the *caught* exception**, not rethrow "something
   with the same tag": the observable difference appears as soon as an exception
   is caught by an inner arm, rethrown, and caught by an outer arm expecting its
   own payload — and again with `catch_all`, which has no tag to reconstruct.
4. **A trap is not an exception**, and a legacy module cannot change that
   (`EXCEPTIONS.md` §3).

## 7. Plan

`LE-0`…`LE-6` are done; `LE-8` is the one open row. The order was: name the
refusal and build the fixtures first (they pay off without committing to
anything), then the rewrite, then the switch that makes it opt-in.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **`LE-0` — Record the analysis** | `✅` This note: the encodings, the gap list, the deviations, and the measured probes. | — | Reviewed, linked from [`EXCEPTIONS.md`](EXCEPTIONS.md) §2 and from the language-study set (`S1`/`S2`/`S4`, `Q-1`). |
| **`LE-1` — Name the refusal** | `✅` `dispatchOpcode` decodes the five opcodes and `legacyExceptionHandlingMessage()` composes the message: the proposal, the instruction, both compiler flags, the Binaryen pass, `WASMJIT_LEGACY_EH`, and this note. **Class A.** | `LE1` | `--diagnose` prints it; `[legacy-eh]` "refused by name" asserts each remedy. |
| **`LE-2` — Fixtures and a repeatable probe** | `✅` Four `.wat` sources plus their committed `.wasm` in `Test/wasm_wat/legacy/`, Binaryen's translated twin, and `Test/legacy-eh-probe.sh` (regenerates the fixtures and prints the `P1`–`P9` matrix, skipping absent tools). | `LE6` | The script runs from a clean tree, and the committed `.wasm` are exactly what the tests load. |
| **`LE-3` — Desugar `try`/`catch`/`catch_all`** | `✅` `BodyRewriter`: instruction spans, the merge block, payload landing blocks, the `try_table`, the arm bodies, the branch remap, branch-hint invalidation. **Class A.** | `LE4`, `LE5` | A legacy module loads and runs with the same results as its `wasm-opt --translate-to-exnref` twin (`[legacy-eh]` "agrees with Binaryen's translation"). |
| **`LE-4` — `rethrow` with payload identity** | `✅` `catch_ref`/`catch_all_ref` on the rethrowing arm, a synthetic `exnref` local, `local.set` at the landing, `throw_ref` at the site. **Class A.** | `LE2` | Same-function, cross-function and void-payload rethrows land in the right arm (`same` = 6, `outer` = 49, `voidpath` = 3). |
| **`LE-5` — `delegate`** | `◑` The non-skipping case ships (a try with no arms becomes a block); the skipping case is refused by name. **Class A.** | `LE3` | `d1`/`d2` pass; `delegate_skip` is refused with a message naming the construct. **`LE8` remains open.** |
| **`LE-6` — The opt-in switch** | `✅` `ParseOptions::legacyExceptionHandling` (parser policy, `WasmParseOptions.hpp`), consulted in `Module::parseBody`; `WASMJIT_LEGACY_EH` only seeds the default overloads. | `LE7` | The default refuses; the policy accepts; `[legacy-eh]` covers both directions and the precedence rule. |
| **`LE-8` — `delegate` that skips a catch** | **Not started.** Needs per-region clause splitting of the enclosing try, i.e. duplicated arm bodies. | `LE8` | A fixture whose delegate target skips an intervening catch runs with the intermediate arm *not* catching. Until then the refusal stays. |

**Where the switch lives, and why not in `RuntimeConfig`.** It is a parser-side
policy: `ParseOptions` ([`WasmParseOptions.hpp`](../../WasmBase/WasmParseOptions.hpp))
mirrors `ValidationOptions`, which already answers the neighbouring question and
already rides on `Module::fromFile`. Putting it there means the decoder can be
widened for one load without widening the validator, and it keeps the decision
with the artifact instead of with the process — `module.parserOptions()` reports
what an artifact was loaded with, which the module cannot otherwise show.

`RuntimeConfig` was deliberately **not** extended. The reason is *not* how it is
passed — `Runtime::create` already takes it by `const&` — but **who builds it**:
the embedder does, in its own translation unit. A new field makes that object
smaller in an embedder compiled against the older header, and the library then
reads past its end; switching the parameter to a reference does not change that,
because the object's size is fixed at the call site. Growing such a struct safely
needs one of three things, none of which exists today: a size/version word inside
it that the callee checks, a reserved tail, or an opaque builder. Until one of
those is a milestone decision, the runtime path uses the environment seed
(`WASMJIT_LEGACY_EH`) and an embedder that needs per-load control calls
`Module::fromFile` with a `ParseOptions` and instantiates it itself. Adding the
field later is Class A *for the API* and a `WASMJIT_ABI_VERSION` decision, which is
why it waits for a named consumer.

## 8. Indicative test survey

Two layers: the committed fixtures and the `[legacy-eh]` test cases, and the
probe matrix that `Test/legacy-eh-probe.sh` reprints on demand.

| Fixture (`Test/wasm_wat/legacy/`) | Exercises | Results the tests assert |
|---|---|---|
| `try_catch.wasm` | `try` with a `catch` and a `catch_all` arm; a void try | `pick(1)` = 101, `pick(2)` = 1000, `pick(3)` = 103, `void_try(1)` = 42 |
| `try_catch.exnref.wasm` | Binaryen's `--translate-to-exnref` of the same source | the same four results, with the switch **off** (it is already final-form) |
| `rethrow.wasm` | `catch_all`+`rethrow` cleanup pad, payload arm rethrow, cross-function rethrow, void rethrow | `same` = 6, `outer` = 49, `voidpath` = 3, and `cleanup` leaves a *pending exception* |
| `shapes.wasm` | branch out of body/arm, a branch inside the body, a `br_table`, two `delegate` depths | `b1` = 11, `b2` = 5, `b3` = 7, `b4` = 22/21/20, `d1` = 9, `d2` = 9 |
| `delegate_skip.wasm` | `delegate` whose target skips a catch | the load is **refused** with "delegate that skips an intervening catch" |

| Probe (`./Test/legacy-eh-probe.sh`) | Result on 2026-10-30 | Reading |
|---|---|---|
| `P1` wabt rebuilds the four fixtures | 106 / 172 / 190 / 63 bytes | the committed `.wasm` are reproducible from the `.wat` |
| `P2` `wasm-tools validate try_catch.wasm` | refused: "legacy_exceptions feature required for try instruction" | final-spec tooling rejects the form by default |
| `P2b` `--features legacy-exceptions` | accepted | the tooling can still inspect it, which is what makes `LE-2` cheap |
| `P3` harness, switch off | refused with the `LE-1` message | the default is strict and instructive |
| `P4` `--translate-to-exnref` | produced a 150-byte twin | the workaround of §10.5 exists |
| `P5` harness, `WASMJIT_LEGACY_EH=1` | `pick(1)` = 101 | the rewrite runs the module |
| `P8` harness, the twin | `pick(1)` = 101 | the rewrite agrees with Binaryen |
| `P6` clang default | 2 legacy `try` blocks, 2 `rethrow`s | the current LLVM still emits the superseded form |
| `P9` clang cleanup pad | 1 `catch_all` | destructor paths use `catch_all`+`rethrow`, not `delegate` |
| `P7` clang `-wasm-use-legacy-eh=false` | 2 `try_table`s | one flag moves a module to the form we already execute |

Default suite: `./Test/WasmJit '[legacy-eh]'` runs seven cases / 60 assertions —
the last one pins the documented precedence (the environment seeds the policy, an
explicit `ParseOptions` overrides it) — and nothing else in the suite changes: no
spec script uses the form, and the exported-symbol baseline gained only the five
policy-carrying overloads
(`abi-check.sh`: 0 removed, 5 added, baseline updated).

Reading: the adapter is tested where it can be — against wabt's own legacy
encoder, against Binaryen's independent translation, and against a hand-written
refusal — and it is not tested where it cannot be: there is no upstream spec
suite for a retired proposal, so `LE-8` and `LE-9` are refusals with fixtures
rather than silent gaps.

## 9. References

- **`R-1`** — [the legacy explainer](https://github.com/WebAssembly/exception-handling/blob/master/proposals/exception-handling/legacy/Exceptions.md)
  (the `WebAssembly/exception-handling` repository, archived 2025-04-25;
  fetched 2026-10-30). Source of §2's encodings, §3's freedoms, and the
  "superseded in Oct 2023 … can be deprecated in future and the use of this
  proposal is discouraged" sentence. It is **the** authority for this note, and
  it is a dead document: nothing will ever be corrected there.
- [the current exception-handling proposal](https://github.com/WebAssembly/exception-handling/blob/main/proposals/exception-handling/Exceptions.md)
  — the final `exnref` shape our tree implements.
- [`EXCEPTIONS.md`](EXCEPTIONS.md) §2/§4/§5 — the in-tree implementation
  (`EX-1`…`EX-7`), the clause model, and the "legacy instructions were removed
  from the proposal" statement this note starts from.
- [`../READINESS_AUDIT.md`](../READINESS_AUDIT.md) §5.2 — the exception proposal
  at 100% in the pinned snapshot.
- [`../ABI.md`](../ABI.md) "Native (host) function imports" — the pending-trap /
  pending-exception boundary an implementation must not disturb.
- [`../SAFETY.md`](../SAFETY.md) §2.1, §3.1 — "tell artifacts apart before
  decoding", and the limits (`S1` no step budget) that a desugaring may not
  change.
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 — the gap classes
  (`A`/`B`/`C`/`D`/`E`) this note's §5 rows and §7 milestones cite.
- [`../LAYERING.md`](../LAYERING.md) §4 — the backend contract; the reason
  `LED2` desugars in the front end instead of adding a dispatch method every
  backend must implement.
- [`../languages/TRIAGE.md`](../languages/TRIAGE.md) §6.8 — `Q-1`, the
  engine question this note answers — and §6.9, the **re-screen** the shipped
  mitigation triggered (two rows re-graded, no cluster or verdict moved), plus
  [`../languages/CHARTERS.md`](../languages/CHARTERS.md) `S1`/`S2`/`S4`, whose EH
  questions are now about the residue (`LE-8`/`LE-9`) rather than the encoding.  engine question this note answers, and [`../languages/CHARTERS.md`](../languages/CHARTERS.md)
  `S1`/`S2`/`S4`, the three charters that would consume the answer.
- [`../languages/EVIDENCE.md`](../languages/EVIDENCE.md) `E-2` (the rustc book
  on LLVM's legacy-EH default), `E-4` (Kotlin/Wasm's documented requirement) and
  `E-8` (`WASM_LEGACY_EXCEPTIONS` exists in Emscripten's settings).
- [Emscripten's C++ exception support page](https://emscripten.org/docs/porting/exceptions.html)
  (fetched 2026-10-30): `-fwasm-exceptions` is an opt-in; the page documents the
  two encodings and the `std::terminate` limitation both share.
- Binaryen 124's `wasm-opt --help`: `--translate-to-exnref` ("translate old
  Phase 3 EH instructions to new ones with exnref") and the deprecated alias
  `--translate-to-new-eh`; `--emit-exnref` and its deprecated alias
  `--experimental-new-eh`.
- `wasm-tools` 1.252.0: `validate --features legacy-exceptions` accepts the
  deprecated form, and the default feature set rejects it with the message quoted
  in §4.5 `P2`.
- **The policy type**: [`WasmBase/WasmParseOptions.hpp`](../../WasmBase/WasmParseOptions.hpp)
  (embedder tier, in the umbrella) — `ParseOptions::legacyExceptionHandling`,
  `ParseOptions::fromEnvironment()`, and the precedence rule the tests pin.
- **The implementation**: [`WasmBase/WasmLegacyEh.hpp`](../../WasmBase/WasmLegacyEh.hpp)
  (internal tier, [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) §6.1.1) and
  [`WasmBase/WasmLegacyEh.cpp`](../../WasmBase/WasmLegacyEh.cpp) — the switch, the
  message, the scanner, the tree and the emitter. The five opcode arms live in
  [`WasmBase/WasmOpcodeDispatcher.cpp`](../../WasmBase/WasmOpcodeDispatcher.cpp),
  and the hook is one call in `Module::parseBody`
  ([`WasmBase/WasmModule.cpp`](../../WasmBase/WasmModule.cpp)).
- **The fixtures and the probe**: [`Test/wasm_wat/legacy/`](../../Test/wasm_wat/legacy)
  (four `.wat`, four `.wasm`, Binaryen's twin),
  [`Test/LegacyEhTests.cpp`](../../Test/LegacyEhTests.cpp) (`[legacy-eh]`, six
  cases) and [`Test/legacy-eh-probe.sh`](../../Test/legacy-eh-probe.sh).
- [`../TESTING.md`](../TESTING.md) — where the `[legacy-eh]` tag and the probe
  script are listed next to the other gates.
- [`../SAFETY.md`](../SAFETY.md) §2.1 — the "tell artifacts apart before
  decoding" rule the switch keeps true: the default decoder still refuses the
  retired form.
- **`M-1`** (measurement, not a document) — clang 23.1.4, 2026-10-30: the default
  of `-fwasm-exceptions` is the legacy encoding; `-mllvm -wasm-use-legacy-eh=false`
  switches it. Recorded in §4.5 `P6`/`P7`/`P8` as a local observation, scoped to
  that one binary.

## 10. The six commissioned questions

### 10.1 Does it conflict with existing binary encodings?

**No — and this is the strongest single result of the study.** The legacy form
touches six primary opcodes: five are unique to it and unassigned in the final
proposal, and the sixth (`throw`) is the *same* instruction in both.

| Byte | Legacy meaning | Final proposal today | This engine today | Conflict? |
|---|---|---|---|---|
| `0x06` | `try` | unassigned | reserved comment, no enum arm | none |
| `0x07` | `catch` | unassigned | reserved comment | none |
| `0x08` | `throw tagidx` | **`throw tagidx` — same instruction** | implemented (`Opcode::Throw`) | none: identical encoding and semantics |
| `0x09` | `rethrow reldepth` | unassigned | reserved comment | none |
| `0x18` | `delegate reldepth` | unassigned | reserved comment ("0x16-0x18 reserved") | none |
| `0x19` | `catch_all` | unassigned | no arm | none |

Nothing else in the legacy form is new: the tag section (code `13`, `attribute:u8`
+ `typeidx`), import kind `4`, the export kind and name subsection `11` are the
final proposal's own shapes, already decoded by `Module::processTagSection` and
`ImportTag` (§4.4). There is no section, no type, no value type and no immediate
that the two proposals spell differently.

What the answer does **not** say is that the bytes are free *forever*:

- **The primary-opcode space is a shared, first-come resource.** `0x06`/`0x07`/
  `0x09`/`0x18`/`0x19` are unassigned *today*; a future proposal may claim one —
  and a decoder that has already taught itself the legacy meaning then has two
  readings of the same byte, which is the one place a real conflict could appear
  later.
- **Mitigation is the opt-in switch (`LED1`), not good intentions.** With the
  switch off the default decoder remains exactly as spec-faithful as it is now;
  with it on, the embedder has asked for the deprecated reading and the engine
  can say so in a diagnostic. `LED1` turns "no conflict today" into "no conflict
  by construction".

### 10.2 Can it be safely included, in spite of being deprecated?

**Yes, under five conditions — and no, if any of them is dropped.** The
conditions are the deviations of §6, stated as gates:

| # | Condition | Why it is the condition |
|---|---|---|
| **C1** | **Default off, opt-in on (`LED1`).** | The spec discouraged the form; an engine that accepts it silently is more permissive than the spec and gives an embedder no way to say "final only". |
| **C2** | **A front-end desugaring, not a second exception runtime (`LED2`).** | A second runtime means a second set of semantics to keep in step with `EXD2`, a second obligation for every backend, and a second source of GC/reference bugs. The desugaring produces the *same* `try_table`/`throw_ref` the tree already tests. |
| **C3** | **Validation parity, not validation theatre (`LED3`).** | The legacy arms must go through the same tag-index, label-range, arity and depth checks, and inherit the same documented operand-stack taint. Decoding without them would be a new hole in `SAFETY.md`'s "tell artifacts apart before decoding". |
| **C4** | **A named refusal for whatever is not right yet (`LED4`).** | `delegate`'s depth-skipping is the one construct where a plausible translation is silently wrong. Refusing it by name, with `LE-5` as the fix, is what keeps "deprecated" from becoming "approximate". |
| **C5** | **Fixtures before the feature (`LE-2`).** | There is no upstream suite to inherit (`LE6`). A deprecated form implemented without a regression set is folklore with a decoder arm. |

With all five, the deprecated form behaves like a **load-time compatibility
adapter**: an input the engine can read, not a capability it advertises. That is
what shipped (§4.6). With any
one missing, the engine either becomes lenient by default (C1), grows a second
runtime (C2), loses its validation story (C3), gets a construct wrong (C4) or
cannot prove it right (C5).

### 10.3 How much of an engine rewrite would it involve?

**Zero of the exception machinery; a front-end pass on top of it.** The split
matters more than the line count:

| Untouched | Why it stays untouched |
|---|---|
| The exception object, the pending-exception slot and their ABI (`WasmExn.hpp`) | The desugaring emits `throw`/`throw_ref`, which already use them |
| Tag identity, imports, exports, the tag section | Byte-identical (§4.4) |
| `try_table` lowering, the compile-time static catch search (`dispatchThrow`), the propagation-to-caller path | The desugared form *is* a `try_table` (§4.2) |
| Every backend and the `WasmBase` ↔ backend contract ([`../LAYERING.md`](../LAYERING.md) §4) | No backend ever sees a legacy opcode (`LED2`) |
| The trap boundary and `SAFETY.md`'s policies | Nothing in the desugaring allocates, jumps or unwinds natively |

| Route | What it is | Size (estimate, not a measurement) | Class |
|---|---|---|---|
| **Route A — rewrite the body bytes** ✅ | A pass over `Module::functionBodies[i].code` before validation: an instruction-length scanner, an emitter for the `block`/`try_table`/`br` shape, a local appended for `rethrow`, branch-hint invalidation. Everything downstream (validator, JIT) sees an ordinary final-form module. | ~700 lines + tests (measured) | **A** |
| **Route B — desugar inside `OpcodeDispatcher`** | No byte emitter, but a decoded-frame→real-depth remap for every branch instruction, in a class the validator *and* every backend share. | ~250–450 lines + tests | **A**, but it perturbs the stream every backend consumes |
| **Route C — native handler arms** | A new control-frame kind with multiple entry points, a validator stack model for inline arms, and a per-backend CFG for them. | a rewrite of the control-flow model, per backend | **E** — fails `G2` on its own |

**Chosen: Route A - and built.** `WasmBase/WasmLegacyEh.cpp` is the whole
rewriter (scan, tree, emit, remap, refusal paths) at roughly 700 lines including
comments, and it changed no backend, no ABI and no exported symbol. Its invariants
are checked in isolation: the fixtures are written by wabt, one is compared with
Binaryen's own translation of the same source, and the refusals have their own
fixtures. The one structural surprise (`LE5`, branch hints) was a
"drop the hints for rewritten bodies" line, as predicted.

Route C is what "how much of a rewrite" means if the question is taken literally,
and the answer is *don't*: the same semantics are available for a fraction of the
cost, one layer earlier.

### 10.4 How useful is it?

Usefulness is not one number — it is a function of *who can recompile the
module*. Measured or documented per consumer, on 2026-10-30:

| Consumer | Emits legacy by default? | Can the module's author switch? | What that means for us |
|---|---|---|---|
| **clang / LLVM 23.1.4** | **Yes** (§4.5 `P6`, measured) | **Yes**, one flag: `-mllvm -wasm-use-legacy-eh=false` (`P7`) | High exposure, trivially avoidable *by the author*. The engine's job is to say so (`LE-1`) |
| **Rust** (`wasm32-unknown-unknown`) | No — `panic=abort` is the default, so there is no EH in the module at all | Unwinding needs nightly, `-Zbuild-std` *and* the same LLVM flag ([`../languages/EVIDENCE.md`](../languages/EVIDENCE.md) `E-2`) | Low: the default path never hits this |
| **Emscripten** | `-fwasm-exceptions` is an opt-in; the encoding is controlled by `WASM_LEGACY_EXCEPTIONS` (`E-8`); the **default was not verified** here (`E3`) | Yes, if the setting is what we think it is | Medium, and worth measuring in the `S1` charter |
| **Kotlin/Wasm** | **Yes** — its own docs require "legacy exception handling" ([`E-4`](../languages/EVIDENCE.md)); no user-facing encoding flag was found | **No.** The Kotlin toolchain decides, not the embedder | **Decisive.** This is the one consumer that cannot be fixed by a compiler flag |
| **Third-party / prebuilt modules** | Unknown, but anything built before the 2023 switch is legacy | **No** — no source, or no build | High, and unmeasurable: exactly the "load someone else's module" case this engine exists for |
| **Swift / D / Ada / Nim via LLVM** | Probably the LLVM default (`E3`, not measured) | Probably the same flag | Medium until measured |

Reading: **the feature is at its most useful exactly where the workaround is
weakest.** For clang users it is a documentation problem; for Kotlin/Wasm and for
prebuilt binaries it is the only way the module loads at all. A study that
concluded "just recompile" would be right for the first group and useless for the
second.

One correction the probes force: the exposure is *wider* than "code that uses
`try`". `P9` shows that a function with a **destructor** — no `catch` anywhere in
sight — already contains `try`/`catch_all`/`rethrow`, so any C++ module built
without `-fno-exceptions` carries the legacy encoding, whether its author thought
about exceptions or not. That is the difference between "a niche feature of a
deprecated proposal" and "the default shape of C++ output".

### 10.5 Are there workarounds if we opt not to include it?

Yes — five of them, and the pattern is that each works exactly where the others
fail:

| # | Workaround | Works for | Fails for | Cost |
|---|---|---|---|---|
| **W1** | **Compiler flag**: `-mllvm -wasm-use-legacy-eh=false` (clang/LLVM), `-sWASM_LEGACY_EXCEPTIONS=0` (Emscripten), the same LLVM flag through `RUSTFLAGS` (Rust unwinding) | Any module whose *author* has the source and a current toolchain — measured: `P7`/`P8` | Kotlin/Wasm (no flag), third-party or prebuilt modules (no source) | free |
| **W2** | **Don't use EH**: `-fno-exceptions`, `panic=abort`, reject-based error handling | Game-style code that can treat errors as traps or error codes | Any code whose cleanup semantics are exceptions (`std::unique_ptr` in a throwing call chain, Kotlin's own runtime) | free, but it removes a language feature rather than supporting it |
| **W3** | **Binary translation**: `wasm-opt --enable-exception-handling --translate-to-exnref in.wasm -o out.wasm` | **Any** module, source or not — measured: `P4` converts, `wasm-tools` validates, and the engine runs the result (`P5`, `i32 = 7`) | Embedders unwilling to add a Binaryen step, and any workflow where a module arrives *at runtime* rather than at build time | one pipeline step |
| **W4** | **Embedder-side load hook**: rewrite the body in the embedder before `loadModule` | Nobody, really | — | It *is* `LE-3`, duplicated per embedder, without the fixtures or the review |
| **W5** | **Wait for the toolchain**: Kotlin/Wasm migrating to the final encoding | A future Kotlin release | Anyone who needs it before that release, and every already-built module | unknown date |

**The honest summary.** W1/W2 are the right answer for the *author of the module*
and cost nothing; W3 is the right answer for *whoever holds the `.wasm`* and
needs no source at all; W4 is the engine work wearing someone else's hat; W5 is
not a plan because it has no date. Everything except W4 is compatible with
`LE-1`'s "name the remedies" milestone, which is why `LE-1` is worth doing even
if nothing else here is.

### 10.6 What is the path of least resistance?

Six options, ordered by what they cost and who pays:

| # | Option | Engine work | Class | Who pays | Verdict |
|---|---|---|---|---|---|
| **O0** | Do nothing | none | — | Every C++/Kotlin user, with a message that explains nothing | Rejected: the status quo already fails, and fails unhelpfully |
| **O1** | **Name the refusal** (`LE-1`): a diagnostic that says "legacy exception handling", names both remedies, links this note | done | **A** | The project, once | **✅ Shipped** |
| **O2** | **Fixtures + a repeatable probe** (`LE-2`): the four sources, their `.wasm`, Binaryen's twin, and `Test/legacy-eh-probe.sh` | done | **A** | The project, once | **✅ Shipped** |
| **O3** | **Front-end desugaring, opt-in** (`LE-3`…`LE-6`, Route A) | done | **A** | The project, once | **✅ Shipped**, minus the two structures `LE-8`/`LE-9` name and refuse |
| **O4** | **Native handler arms in every backend** (Route C) | a control-flow-model rewrite | **E** | The project, per backend | **Rejected outright** — not deferred, not "later": it restructures the control-flow model to buy what `O3` already delivers, and it fails `G2` on its own. `WasmBase/` will not grow a second exception model |
| **O5** | **Embedder pre-pass with `wasm-opt`** (W3) | none | — | The embedder | **Documented fallback**, already proven |

**Outcome.** `O1`, `O2` and `O3` are shipped; `O4` is rejected outright. The
order was deliberate: `O1`/`O2` first (a message that names the remedy, and a
fixture set that makes the rest reviewable), then `O3` behind the switch. The
residual work is `LE-8` (a `delegate` whose target skips a catch) and `LE-9`
(a body that mixes the two forms), both refused by name today; neither justifies
a change until a real module hits one.

**Why `O4` is not "deferred".** Deferring it would leave the door open for a
future "let us do it properly" pass that duplicates the exception model. The
ship proves the point: the same semantics were available one layer earlier for
a fraction of the cost, and every backend stayed untouched. A second exception
model inside `WasmBase/` is not a scheduling question.

**What would change this recommendation, in order of likelihood:**

1. A Kotlin/Wasm embedder, or a game shipping prebuilt C++ mods — then `O3`
   becomes the shortest path, and `O2`'s fixtures are already in place.
2. Kotlin migrating to the final encoding — then `O3` drops to "not worth it" and
   `O1`+`O5` are the whole answer.
3. A future proposal claiming `0x06`/`0x07`/`0x09`/`0x18`/`0x19` — then the
   opt-in switch (`LED1`) becomes mandatory rather than prudent, and §10.1's last
   paragraph becomes the reason it exists.

**How this connects to the language studies.** [`../languages/TRIAGE.md`](../languages/TRIAGE.md)
§6.8 raised this as `Q-1` ("accept legacy EH, or keep requiring final EH?") and
called it `LF-3`-class work. This note is the answer to the *first half* of that
question — what it would cost and whether it is safe — and it deliberately does
not decide it: `Q-1` stays open until a charter produces a PoC that cannot be
built any other way. What the note does do is make the decision cheap when it
arrives: `O1`+`O2` now, `O3` behind a switch if and only if someone asks, and no
`O4` ever.









