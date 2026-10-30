# Legacy Exception Handling (`try`/`catch`) — Compatibility Notes, Encodings, and the Cost of Admission {#prop_legacy_exceptions}

Status: design note / compatibility study (revision 1 — 2026-10-30). It analyses
the **superseded Phase-3 ("legacy") exception-handling binary form**, what it
would cost to accept, and whether we should — and it answers the six questions it
was commissioned with in §10. **Nothing is implemented and nothing is shipped.**
The current recommendation is *document, diagnose, and do not implement yet*
(§10.6). It contains **no new code**.

Audience: anyone touching the exception front end
(`WasmBase/WasmOpcode.hpp`, `WasmOpcodeDispatcher`, `WasmValidator`, LibJit's
`try_table` lowering), the language-feasibility charters `S1`/`S2`/`S4`
([`../languages/CHARTERS.md`](../languages/CHARTERS.md)), and anyone whose C++ or
Kotlin/Wasm module loads here and fails with "Invalid or unsupported primary
opcode".

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
| §5 | Where do we fall short (`LE1`–`LE7`)? |
| §6 | Where may we play fast and loose (`LED1`–`LED5`)? |
| §7 | Plan (`LE-0`…`LE-6`) |
| §8 | Indicative test survey — including nine measured probes |
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

The failure is `OpcodeDispatcher::dispatchOpcode`'s `default:` arm:
`throw std::runtime_error("Invalid or unsupported primary opcode")`. It is
correct, and it is the least helpful message a C++ or Kotlin user can get — the
first gap in §5.

### 4.4 The tag surface is already the legacy surface

`Module::processTagSection` reads `attribute:u8` (currently always `0x00`) then
`typeidx`; `Tag`, `ImportTag` and the export kind are the same shapes the legacy
explainer specifies (§2.1). **A legacy module's tag section, imports, exports and
names need no new decoding at all.**

### 4.5 What the engine does with real modules — measured, twice

Run on 2026-10-30 against this branch's build
(`build/Desktop-Debug/Test/WasmJit`), with wabt 1.0.41, Binaryen 124,
wasm-tools 1.252.0 and clang 23.1.4. The artifacts are study artifacts in `/tmp`,
deliberately not committed.

| # | Probe | Result |
|---|---|---|
| **P1** | a hand-written legacy module (`try`/`catch $e`/`catch_all`, one tag with an `i32` payload) built with `wat2wasm --enable-exceptions` | built; `wasm-tools print` shows `try` / `catch 0` / `catch_all` |
| **P2** | `wasm-tools validate legacy.wasm` | **fails**: "legacy_exceptions feature required for try instruction (at offset 0x29)". With `--features legacy-exceptions` it **validates** — tooling can inspect the form |
| **P3** | engine: `WasmJit --diagnose=legacy.wasm` | **cannot load**: "function 0 at instruction 0: Invalid or unsupported primary opcode" |
| **P4** | `wasm-opt --enable-exception-handling --translate-to-exnref legacy.wasm -o exnref.wasm` | **succeeds**; `wasm-tools validate exnref.wasm` passes; the body becomes `block` / `block` / `try_table (catch 0 …) (catch_all …)` + `br` |
| **P5** | engine: `WasmJit --diagnose=exnref.wasm --invoke=f --args=0` | **loads, instantiates, runs**: `result[0] i32 = 7` |
| **P6** | `clang++ --target=wasm32-unknown-unknown -fwasm-exceptions -O1 -c` on a `try`/`catch(E&)` function | emits **legacy** `try` / `catch 0` / `rethrow 0` — the *default* of LLVM 23.1.4 in 2026 |
| **P7** | the same with `-mllvm -wasm-use-legacy-eh=false` | emits `try_table (catch_ref 0 …)` / `throw_ref` |
| **P8** | engine: `--diagnose` on the P6 object and on the P7 object | P6: "Invalid or unsupported primary opcode"; P7: **loads**, 8 imports (`env.__cpp_exception`, `env.__cxa_*`, …) |
| **P9** | `clang++ … -fwasm-exceptions` on a function with a **destructor** and no `catch` (`struct G { ~G(); }; void f() { G g; may_throw(); }`) | emits `try` / `catch_all` / **`rethrow 0`** — a cleanup pad. `delegate` does **not** appear |

P4+P5 are the whole answer to "are there workarounds" (§10.5): one Binaryen pass
turns the rejected form into the accepted one, and the engine runs the result.
P6 is the answer to "how useful is it" (§10.4): as of 2026 the current LLVM
**still defaults to the legacy encoding**, so this is not a museum piece. P9
corrects the assumption this study started with, and it changes the priorities:
**`rethrow` — not `delegate` — is on the hot path of ordinary C++**, because
every function with a destructor needs a cleanup pad that rethrows.

## 5. Gaps

| # | Gap | Evidence | Severity |
|---|---|---|---|
| **LE1** | **The front end refuses the legacy form with a generic error.** Five opcodes (`0x06`, `0x07`, `0x09`, `0x18`, `0x19`) reach `dispatchOpcode`'s `default:` arm, so a C++/Kotlin user is told "Invalid or unsupported primary opcode" with no hint that a one-flag recompile or a Binaryen pass exists. **Class A** (a decoder arm and a message; no semantics). | §4.3, §4.5 `P3`; `WasmBase/WasmOpcodeDispatcher.cpp` `default:` arm | **High** — it is the first thing an LLVM-built module hits |
| **LE2** | **`rethrow` has no expression in the final form without new state — and it is on the hot path.** It reads the implicit "exception caught by the enclosing catch", which does not exist in the final model; expressing it needs `catch_ref` *plus* somewhere to keep the reference (a synthetic local), because a legacy catch arm cannot observe an `exnref`. **Measured:** clang emits `try`/`catch_all`/`rethrow 0` for a function with a destructor and no `catch` (§4.5 `P9`), so this is not a corner case — it is every C++ cleanup pad. **Class A**, but the only opcode that needs extra storage. | §2.3 item 3; [`EXCEPTIONS.md`](EXCEPTIONS.md) §4 (`throw_ref` exists, "current exception" does not); §4.5 `P9`; Binaryen's output in `P4` has this shape | **High** |
| **LE3** | **`delegate`'s relative depth can skip intermediate catch frames, and the obvious translation is wrong.** "Fall out of the block and let the enclosing handler take it" is only correct when nothing catches in between; when the depth skips a frame, the desugaring has to stop those frames catching *this* try's exceptions. **Class A**, but the one place a desugaring can be silently wrong. Frequency is *low* in current clang output — cleanup pads use `catch_all`+`rethrow` (`P9`), not `delegate` — which is a reason to refuse it first (`LED4`), not a reason to approximate it. | §2.3 item 2; §4.5 `P9` | **High** for fidelity, Low in current clang output |
| **LE4** | **There is no label-depth remap and no module rewriter.** Branch depths inside a legacy `catch` arm are counted against *one* frame per `try`; a desugaring inserts two or three frames, so every `br`/`br_if`/`br_table`/`br_on_*` inside an arm needs a remapped depth. The tree has neither a depth-remap concept in `OpcodeDispatcher` nor a byte-level module rewriter. | `OpcodeDispatcher::dispatchBr` passes `LabelIdx` through unchanged; `Module::functionBodies[i].code` is a raw opcode vector (`WasmModule.cpp` `processCodeSection`) | Medium — the natural home exists (`body.code`), the encoder does not |
| **LE5** | **A body rewrite invalidates branch-hint offsets.** `processCodeSection` rebases `metadata.code.branch_hint` records onto `FunctionBody::code`; rewriting a body shifts every offset after the first `try`. | `WasmModule.cpp` `processCodeSection` step 3 | Low |
| **LE6** | **No legacy fixtures, and no upstream suite to borrow.** `Test/wasm_wat/` holds the final shape only (`exnref_table.wat`); the curated `wast_supported.txt` has no legacy script; the proposal's own repository was archived in 2025, so its tests can be vendored but never updated. | `ls Test/wasm_wat`, `grep -i legacy Test/wast_supported.txt` (no hits); the spec repo's archive banner | Medium — a feature without fixtures is a rumour |
| **LE7** | **The default configuration must not accept deprecated input silently.** Accepting `0x06`/`0x07`/`0x09`/`0x18`/`0x19` unconditionally would make WasmJit *more* permissive than the spec that deprecated them, with no way for an embedder to say "final only". **Class D** until a decision is recorded, then **Class A**. | [`../SAFETY.md`](../SAFETY.md) (tell artifacts apart before decoding); §10.2 | **High** (policy) |

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
| **LED1** | **Accept the legacy form only behind an opt-in switch; the default stays strict.** | §3 leaves scope to the implementation, and the proposal deprecates itself, so "off unless asked" is the honest default. It also keeps [`../SAFETY.md`](../SAFETY.md)'s "tell artifacts apart before decoding" true: the *default* decoder still refuses a form the spec has retired. | A future embedder can load old modules without making every other embedder accept deprecated input, and no policy has to be reversed later. |
| **LED2** | **Desugar in the front end; never let a backend see a legacy opcode.** | §3 imposes no unwinding mechanism, only observable control flow, and every legacy construct reduces to `try_table` + branches, which the engine already executes (`EX-1`…`EX-7`). | Zero duplication across LibJit/SLJIT/AsmJit, one place to test, and the gap stays **Class A** instead of "Class A in the interface plus Class B in every backend". The alternative — native handler arms — restructures the control-flow model and is **Class E** (§10.3). |
| **LED3** | **Validate the legacy form with the same operand-stack taint the final form already uses.** | The catch payload's stack effect is *already* unmodelled for `try_table` (`typeTainted`); tag indices, label ranges, arm result arity and branch depths are still checked. | No second, better validator has to be built for a deprecated form. |
| **LED4** | **Refuse `delegate` with a *named* error until it is right (`LE-5`).** | **Measured:** clang 23 emits `catch_all` + `rethrow` for cleanup pads, not `delegate` (§4.5 `P9`), so refusing it costs little in *current* output — and a construct whose skipping semantics is not implemented must not be approximated, because older LLVM output and the proposal's own test suite both contain it. `rethrow` is **not** in this bucket: it is on the hot path (`LE2`) and ships with `LE-3`. | `try`/`catch`/`catch_all`/`rethrow` ship first; `delegate` gets its own milestone, fixture and review. |
| **LED5** | **Do not materialise an `exnref` for a legacy catch unless a `rethrow` needs it.** | §3: a legacy module cannot observe an exception value at all, so retaining one is unobservable. | The fast path (no `rethrow` in the arm) stays a plain `catch` label carrying the payload as branch arguments — the existing lowering, untouched. |

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

`LE-0` is this note. `LE-1`/`LE-2` are cheap and worth doing without a requester;
`LE-3`…`LE-6` are gated on one, because they are the point where a deprecated
form starts costing product code.

| Milestone | Work | Closes | Done when |
|---|---|---|---|
| **`LE-0` — Record the analysis** | `✅` This note: the encodings, the gap list, the deviations, and the measured probes `P1`–`P9`. | — | Reviewed, linked from [`EXCEPTIONS.md`](EXCEPTIONS.md) §9 and from the language-study set (`S1`/`S2`/`S4`, `Q-1`). |
| **`LE-1` — Name the refusal** | A dedicated diagnostic for `0x06`/`0x07`/`0x09`/`0x18`/`0x19` that says *which* superseded proposal the module uses and names both remedies (the compiler flag and the `wasm-opt` pass). Docs: a short section in [`../languages/CHARTERS.md`](../languages/CHARTERS.md)'s shared-investigations block and in this note. **Class A**, no semantics. | `LE1` | `--diagnose` on a legacy module prints a message containing "legacy exception handling" and a remedy; a test asserts the wording. |
| **`LE-2` — Fixtures and a repeatable probe** | Vendor a curated subset of the archived proposal's legacy `*.wast` tests; keep them out of `wast_supported.txt` (they are not final-spec scripts) and add a small test that expects the *named refusal*. Record `P1`–`P8` as a documented probe script so the next reader can rerun them. | `LE6` | The fixtures exist, the refusal test runs in the suite, and the probe script reproduces `P4`+`P5` on a clean machine. |
| **`LE-3` — Desugar `try`/`catch`/`catch_all` (+ `rethrow`)** | The rewriter: an instruction-length scanner over `FunctionBody::code`, an emitter for the `block`/`try_table`/`br` shape, the label-depth remap, and branch-hint invalidation for rewritten bodies. `rethrow` is part of this milestone, not a follow-up: a C++ module needs it for every cleanup pad (§4.5 `P9`), so `LE-3`+`LE-4` are the first *usable* pair. `delegate` stays refused (`LED4`). **Class A.** | `LE4`, `LE5` | A legacy module using `try`/`catch`/`catch_all`/`rethrow` loads and runs here with the same result as its `wasm-opt --translate-to-exnref` twin. |
| **`LE-4` — `rethrow` with payload identity** | Convert the enclosing arm's clause to `catch_ref`/`catch_all_ref`, append a synthetic local to the body's locals, store the reference at arm entry, and lower `rethrow` to `throw_ref`. (Split out of `LE-3` so the rewriter can land first; the two ship together.) | `LE2` | A fixture where an inner arm rethrows and an outer arm catches the *same* exception (payload identity preserved) passes; a `catch_all` rethrow does too. |
| **`LE-5` — `delegate`** | Implement the depth-correct translation: the target frame's skipping must be honoured, including when an intervening `try` would otherwise catch. | `LE3` | A fixture whose `delegate` target skips an intervening `try`/`catch` proves the intermediate arm does **not** catch. Until then `LED4` refuses it. |
| **`LE-6` — The opt-in switch** | A load-time switch (an env var or an additive `LoadOptions` field), documented in [`../EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) and [`../VALIDATION.md`](../VALIDATION.md); the default stays "final only". **Class A** for the field, and a milestone decision because it extends an embedder-facing surface. | `LE7` | The default refuses a legacy module; the switch accepts it; `[validation]`/`[runtime]` cover both directions. |

**Why the order is this one.** `LE-1`/`LE-2` are the parts that pay off without
committing to anything (a better error and a fixture set). `LE-3` is the only
piece that has to be got right structurally; `LE-4`/`LE-5` are opcode-sized
increments on top; `LE-6` is what makes the whole thing opt-in rather than a
silent policy change, so it must land *with or before* `LE-3`, not after.

## 8. Indicative test survey

No legacy script exists in the tree, so the table is probe-shaped rather than
script-shaped: each row is a command this study actually ran on 2026-10-30
(§4.5), with its counts and its reading.

| Script / probe | passed | failed | skipped | Reading |
|---|---:|---:|---:|---|
| `wasm-tools validate legacy.wasm` | 0 | 1 | 0 | expected: final-only validation refuses `try` with "legacy_exceptions feature required" |
| `wasm-tools validate --features legacy-exceptions legacy.wasm` | 1 | 0 | 0 | the tooling *can* inspect the deprecated form, which is what makes `LE-2`'s fixtures cheap |
| `wasm-tools print legacy.wasm` | 1 | 0 | 0 | the text form is readable: `try` / `catch 0` / `catch_all` |
| `WasmJit --diagnose=legacy.wasm` | 0 | 1 | 0 | our refusal, with the generic message `LE1` is about |
| `wasm-opt --translate-to-exnref` then `wasm-tools validate` | 2 | 0 | 0 | the conversion exists and its output validates — the workaround of §10.5 |
| `WasmJit --diagnose=exnref.wasm --invoke=f --args=0` | 1 | 0 | 0 | the translated module loads, instantiates and returns `i32 = 7` |
| `clang++ … -fwasm-exceptions` (no extra flag) | 1 | 0 | 0 | current LLVM emits the **legacy** form (`P6`), so the refusal is reachable today |
| `clang++ … -mllvm -wasm-use-legacy-eh=false` | 1 | 0 | 0 | the final form is one flag away, and we load it (`P8`) |
| curated spec suite (`Test/wast_supported.txt`) | 0 | 0 | all | there is no legacy script to run; the 3.0 suite did not have one |
| `Test/wasm_wat/exnref_table.wat` | 1 | 0 | 0 | the tree tests the final shape only |
| `./Test/WasmJit '[docs]'`, `'[headerdoc]'` | 2 | 0 | 0 | this note adds documentation only; the note-tree check counts it |

Reading: nothing in the default suite changes, and the study's evidence is a set
of commands anyone can rerun rather than an assertion. When `LE-3` lands, its
rows move from "probe" to "test", and `LE-2`'s fixtures become the regression
set.

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
  engine question this note answers, and [`../languages/CHARTERS.md`](../languages/CHARTERS.md)
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
adapter**: an input the engine can read, not a capability it advertises. With any
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
| **Route A — rewrite the body bytes** | A pass over `Module::functionBodies[i].code` before validation: an instruction-length scanner, an emitter for the `block`/`try_table`/`br` shape, a local appended for `rethrow`, branch-hint invalidation. Everything downstream (validator, JIT) sees an ordinary final-form module. | ~400–700 lines + tests | **A** |
| **Route B — desugar inside `OpcodeDispatcher`** | No byte emitter, but a decoded-frame→real-depth remap for every branch instruction, in a class the validator *and* every backend share. | ~250–450 lines + tests | **A**, but it perturbs the stream every backend consumes |
| **Route C — native handler arms** | A new control-frame kind with multiple entry points, a validator stack model for inline arms, and a per-backend CFG for them. | a rewrite of the control-flow model, per backend | **E** — fails `G2` on its own |

**Recommendation: Route A.** Its invariants are testable in isolation (the output
must validate with `wasm-tools`, and must behave like its
`wasm-opt --translate-to-exnref` twin), it leaves the hot path and the backend
contract alone, and its one structural surprise (`LE5`, branch hints) is a "drop
the hints for rewritten bodies" line rather than a design problem.

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
| **O1** | **Name the refusal** (`LE-1`): a diagnostic that says "legacy exception handling", names both remedies, links this note | ~50 lines + docs (estimate) | **A** | The project, once | **Do now** |
| **O2** | **Fixtures + a repeatable probe** (`LE-2`): vendor the archived tests, record `P1`–`P9` | tests only | **A** | The project, once | **Do now** (cheap, and it makes `O3` reviewable) |
| **O3** | **Front-end desugaring, opt-in** (`LE-3`…`LE-6`, Route A) | ~400–700 lines + tests | **A** | The project, once | **On a requester** — the only fix for Kotlin/Wasm and prebuilt binaries |
| **O4** | **Native handler arms in every backend** (Route C) | a control-flow-model rewrite | **E** | The project, per backend | **No** |
| **O5** | **Embedder pre-pass with `wasm-opt`** (W3) | none | — | The embedder | **Documented fallback**, already proven |

**Recommendation.** Take `O1` and `O2` now: together they turn the worst failure
mode (a real C++ or Kotlin module, a message that says nothing) into a message
that says exactly what to do, and they cost no semantics. `P9` raises the urgency
of `O1` specifically: C++ users hit the refusal **by default**, with no `try` in
their source, so the message has to name the flag rather than the opcode. Leave
`O3` for the day a requester appears — and treat **Kotlin/Wasm's `S4` charter** as
the most likely requester, because it is the only consumer that cannot use `O5` or
W1. Reject `O4` permanently: it buys nothing `O3` does not, and it fails the
milestone's own gate on its own (`API_ABI_STABILITY.md` §4.1, class E).

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









