# `F2` — Legacy `delegate` (`LE-8`): Workaround and Long-Term Options {#lf2_deep_f2}

Status: deep-study note (revision 2 — 2026-10-30: **`F2` is fixed.** `LE-8` now
translates the delegating shape in place, so `T05` (C++ with libc++, default
flags) is green without a flag and without `wasm-opt`; revision 1 analysed the
workarounds and concluded the shape needed a translation pass — §4/§4.1 record
why the earlier analysis read the construct as impossible and which mechanism
made it a local rewrite after all). It investigates `F2` of
[`LF2_INDEX.md`](LF2_INDEX.md) §5: a default `clang++` C++ module did not load
because its legacy exception encoding contains a shape
`ParseOptions`'s rewrite refused (`LE-8` of
[`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6). It
verifies the `wasm-opt --translate-to-exnref` workaround end to end, states the
toolchain flags that avoid the shape, and records the in-engine translation that
replaced all of them as the *only* step an embedder needs.

Audience: whoever owns `WasmBase/WasmLegacyEh.cpp`, whoever writes the C/C++ or
Kotlin embedding recipe, and `LF-3` when it prioritises the EH work.

**Headline.** Both workarounds still work (translated module loads, constructor
runs, guest `try`/`catch` returns 42/−1), and both are now optional: the engine
translates clang's default output itself. `t05_eh.wasm` — 336 KB, 7 functions,
libc++/libc++abi/libunwind/setjmp — loads under the *default* `ParseOptions`
recovery policy and runs its exceptions end to end, with
`WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN` as the only trace.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What the legacy construct is, and why the rewrite refused it when it did |
| §3 | The workarounds, verified |
| §4 | Why the skip looked like a large change, and the escape block that made it a local one (§4.1) |
| §5 | The long-term options and what is now implemented |
| §6 | What changed in the report set because of this study |
| §7 | What this note does not claim |

## 2. The construct

The failing module is `t05_eh.wasm`: clang 23.1.0 (wasi-sdk 34.0) with
`-fwasm-exceptions`, i.e. LLVM's **legacy** default. `wasm-tools print` shows the
shape:

```wat
try ;; outer, has catch arms
  ...
  try ;; middle
    try ;; inner
      ...
    delegate 6        ;; propagates past the middle try
    ...
  ...
catch 0
```

The `delegate 6` says: an exception raised by the inner body is delivered six
label-levels out, **skipping** the enclosing legacy `try` that has catch arms.

The engine's rewrite ([`WasmLegacyEh.cpp`](../WasmBase/WasmLegacyEh.cpp)) lowers
a delegate-only legacy `try` to a plain `block`:

```cpp
// A `delegate` (or a try that catches nothing): the body's exceptions must
// reach the frame at `delegateDepth`. A plain block is correct unless a legacy
// try with arms sits in between — then the target would be skipped and the
// intervening arms would wrongly catch.
for (std::size_t k = 1; k < depth; ++k)
	if (isCatchFrame(binToken_[...]))
		refuseRewrite("delegate that skips an intervening catch");
```

That refusal was **sound for the code that raised it**. A plain block has no
effect on exception routing, so the intervening `try_table` — which the rewrite
produced for the middle try's arms — would catch an exception the original module
says must skip it. Silently accepting the module would have changed program
semantics. It was not, however, the end of the story: §4.1 records the mechanism
that routes the exception past those frames without restructuring the function.

**The premise in `LF-1` needs one correction.** The screen recorded Kotlin/Wasm
as the row that *depends* on legacy EH. Kotlin/Wasm **2.4.21** emits
`try_table` (10 occurrences, zero legacy `try` in `wasm-wasi`), so for that
toolchain the premise is stale. What still emits legacy EH among the tested
arms is clang/C++ (default), Lua's `setjmp` lowering, and Dart's runtime
(`t20_dart.wasm` has two legacy `try`). That changes who `F2` blocks, not
whether it matters.

## 3. The workarounds, verified

### 3.1 Toolchain flags (preferred)

| Toolchain | Flag | Effect |
|---|---|---|
| clang / wasi-sdk | `-mllvm -wasm-use-legacy-eh=false` | emits `try_table`; verified in `S1` (`t05_final_eh.wasm`, 21 `try_table`, loads and runs) |
| Emscripten | `-sWASM_LEGACY_EXCEPTIONS=0` | same, per the engine's own error text |
| Rust | `-Cllvm-args=-wasm-use-legacy-eh=false` (with `-Zbuild-std` for unwinding) | same; not exercised in `LF-2` |

These are the documented ways to produce a module the engine loads **without**
relying on the rewrite. The `S1` recipe now records them.

### 3.2 One-time translation (prebuilt modules)

```
wasm-opt --translate-to-exnref --enable-exception-handling t05_eh.wasm -o t05_x.wasm
```

Verified: the translated module (336,365 bytes) loads and runs through the
documented recipe:

```
call _initialize/void() -> void
call ctor_value/i32() -> i32=40
call throw_catch/i32(21) -> i32=42
call throw_catch/i32(-1) -> i32=-1
memory memory: 131072 bytes (2 pages) max=4294967296 shared=0
```

`wasm-opt` is already a build dependency of the repository (`Test/node_modules`,
version 124), so this is not a new tool for the project — only a new step for an
embedder that receives a prebuilt legacy-EH module.

### 3.3 What this means for the report set

`T05` is **green**: the same module now loads with no flag and no `wasm-opt`
step, because the engine translates the shape (§4.1). The flag and the
translated twin remain useful — the flag produces a smaller final-form module and
is what a source build should prefer — but neither is required for the verdict,
and neither is required for a prebuilt module.

## 4. Why "just extend the rewrite" looked like a large change

The obvious ask — "make the rewrite handle `delegate` that skips a catch" — looks
like it needs a structural pass, because the rewritten form has no instruction
that says *skip*:

- A legacy `try … delegate N` becomes a `block`, which routes exceptions to the
  enclosing `try_table`s. There is no "propagate past this handler" bit.
- The handler the exception must skip is a `try_table` clause. A raised
  exception is matched by the innermost clause whose tag matches; a catch-all
  matches everything. Making the intervening clause skip one region therefore
  looked like it required either
  (a) **structural hoisting** — moving the delegated body out of the
  intervening `try_table`'s syntactic extent, or
  (b) **filtering** — catching everything and rethrowing unless the exception
  originated in a delegated region, which needs a per-exception identity the
  legacy proposal never had.

Both are avoidable, and §4.1 shows how. The rejected reading was that the
exception had to travel *on the stack* through the frame being left; if it
travels in a local instead, the branch needs no operands and the frame's result
type stops being an obstacle.

### 4.1 The escape block: how the skip became a local rewrite

The working translation, now `LE-8` in `WasmBase/WasmLegacyEh.cpp`:

1. **Resolve the target against the binary label stack**, not the emitted one.
   `delegate l` counts binary labels: `0` is the delegating `try` itself, `l` is
   the named frame — and a label that is not a `try`, or one out of range, means
   *the caller*, which skips every catch frame in the function.
2. **Mark every catch frame in between** (a legacy `try` with arms, or a
   `try_table` with clauses). Frames that catch nothing are transparent, which is
   why `d1`/`d2` still take the plain-block path.
3. **Wrap each marked frame's `try_table` in a `block` with an empty type** — an
   *escape block*. Its id is recorded per frame, and its exit is emitted right
   after the block: `local.get $exn; throw_ref`.
4. **Protect the delegated body** with `try_table (catch_all_ref $landing)` where
   `$landing` is a `block (result exnref)`: every exception from the body — from
   its own instructions or from nested tries — lands there.
5. **Carry it past the frames in a local**, then branch to the *outermost* marked
   frame's escape block. One branch suffices: it unwinds every `try_table` inside
   that block, and a branch never enters a handler.
6. **Rethrow at the escape block's exit.** That point is outside the skipped
   `try_table` but inside every frame the delegate did not skip, which is exactly
   the legacy delivery point: the target `try`'s clauses catch it if they match,
   and otherwise it continues outward — or, for a caller delegate, it leaves the
   function.

Type-wise the frame's merge block still receives its results only on the normal
path (`br`), and the escape path ends in `throw_ref`, which is stack-polymorphic,
so both paths type-check against the frame's declared type. One `exnref` local
per function is enough: an escape never nests, because it ends in a branch.

**Verified against clang's own output**, not only against a hand-written
fixture: `t05_eh.wasm` (§2) loads through the default policy and returns
`ctor_value` = 40, `via_host(20,22)` = 42, `sum_vector(4)` = 10,
`throw_catch(5)` = 10 and `throw_catch(-3)` = −1 — the last one thrown and caught
by libc++'s `__cxa_*` machinery inside the guest. Four fixtures pin the
semantics: `delegate_skip` (`skip` = 9, so the middle `catch_all` did not take it;
`to_caller` and `from_arm` leave the exception pending — the second one covers a
delegate raised *inside* a handler, which the try owning that handler cannot
catch), `delegate_skip_table` (`skip_table` = 1, so the final-form `try_table` did
not take it), and `d1`/`d2` for the non-skipping path.

**A cross-check that was, and remains, inconclusive.** A minimal
`try (try (throw) delegate 1) catch $t` module run through our rewrite and
through `wasm-opt --all-features --translate-to-exnref` disagreed once
(the rewrite returned 99, Binaryen's output 0 under the engine). That was
recorded in revision 1 as a reason to keep refusing; with the fixtures above and
clang's real module passing, the disagreement is more likely a property of that
hand-built Binaryen twin than of the frame accounting, but it was not re-run and
this note does not claim it was.

## 5. The long-term options

| Option | Shape of the work | Class | Verdict |
|---|---|---|---|
| **A. The rewrite is the mechanism** — the loader translates every legacy shape, including the skip; recovery is the default policy and reports itself | done: `resolveDelegates`, `emitEscapeRethrow`, one `exnref` local, `[legacy-eh]` fixtures | **A** | **implemented** (revision 2) |
| **B. An in-engine `--translate-to-exnref` equivalent** — a function-wide restructuring pass | the escape block makes it unnecessary for the skip; the remaining difference from Binaryen is *shape*, not expressiveness | **A** | **not needed** |
| **C. Toolchain flags / `wasm-opt` for embedders** | documentation only | **D** | **optional** — still the smaller module, no longer a requirement |
| **D. Runtime support for the legacy encoding** | the legacy instructions are superseded; the validator and every backend would need them | **C/E** | rejected |

**Why A supersedes the earlier recommendation.** Revision 1 recommended "final EH
is the supported encoding; legacy EH is a best-effort convenience", because the
skip looked inexpressible without a structural pass. It is expressible: a branch
plus one local does it, in the same pass, with no change to the validator, the
backend contract or any frozen surface. The consequence for an embedder is that
clang's default output — the one a user gets without knowing about
`-wasm-use-legacy-eh=false` — loads, and the flag becomes a size preference.

### 5.1 Should the rewrite be opt-out instead of opt-in?

The natural follow-up question is whether `ParseOptions::legacyExceptionHandling`
should default to **on** — "one boolean". The answer from the `LF-2` data is
**no**, because it changes no verdict:

| Arm | Emits legacy EH? | Would a default-on rewrite help? |
|---|---|---|
| Kotlin/Wasm 2.4.21 (`wasmWasi`) | **no** — `try_table` (10 occurrences, zero legacy `try`) | no; it is green without the rewrite |
| Go 1.27.3 (`wasip1`) | no | no |
| Rust 1.99.1, Zig, C (freestanding / wasi-sdk) | no (C++ only through libc++) | no |
| Lua 5.4.8 (`setjmp`) | yes | it already runs **with** the rewrite; defaulting it on removes an env var, not a colour |
| C++ 20 / clang default | yes, **and** the `delegate`-skip shape | yes — the skip is translated now, so the default policy loads clang's output; verified on `t05_eh.wasm` |
| Dart 3.13.7 | yes | no — the rewrite accepts the encoding and then reports `canonical type reference is not declared by this module` |

So the flip alone is a **friction** change, not a support change: it would spare
Lua users a `WASMJIT_LEGACY_EH=1`, and it would make the decoder silently accept
a form the spec has retired — the exact permissiveness
[`SAFETY.md`](../SAFETY.md) §2.1 warns about, and the reason the option was
strict-by-default in the first place.

**What was implemented instead** (`LE-6`, revision 4): a **recovery policy**,
not a blanket flip. `ParseOptions::legacyExceptionRecovery` is **on by
default**; it rewrites *only* the bodies that use the superseded form and emits
a `WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN` Note. Strictness
(`legacyExceptionRecovery = false`, `WASMJIT_LEGACY_EH=0`) still refuses with the
named message, and unconditional rewriting
(`acceptingLegacyExceptionHandling()`, `WASMJIT_LEGACY_EH=1`) still accepts
quietly. That keeps "never silently more permissive" — the recovery is a
recorded event, not a silence — removes the knob for the modules the rewrite
*can* handle, and leaves a module the rewrite genuinely cannot translate (Dart's,
for a different reason) failing with its own, accurate error.

The implementation is the recovery *outcome* without a second parse: a decoded
body is checked for the five superseded opcode bytes first
(`mayUseLegacyExceptionHandling()`), and only a body that carries one — the
superseded opcodes are single primary bytes that appear literally — reaches
`rewriteLegacyExceptionHandling()`. A final-form module therefore scans its body
bytes and stops; a legacy module is rewritten once, before `validate()`, exactly
as the explicit switch always did. Re-parsing the artifact was rejected because
it needs a seekable device and doubles the load's I/O for no semantic gain.

Measured cost of the extra pass (the `--call`-free load of three real modules,
best of five, forcing the scan with `WASMJIT_LEGACY_EH=1` against the default
policy, which is its upper bound because candidate bytes are common):

| Module | Size | Default (byte pre-check) | Forced scan | Delta |
|---|---|---|---|---|
| Go 1.27.3 `wasip1` | 2.42 MB | 193.6 ms | 195.8 ms | 2.2 ms (~1 %) |
| Kotlin/Wasm `wasmWasi` | 615 KB | 63.6 ms | 64.2 ms | 0.6 ms (~1 %) |
| C++ default flags (`t05_eh`) | 36 KB | 14.6 ms | 14.5 ms | unmeasurable |

Measured after the change (2026-10-30):

| Arm | Result |
|---|---|
| `T26` Lua (`setjmp`) | green **with no environment variable**; the Note names the body count |
| `T05` C++ default flags | **green** — `LE-8` translates the skip, so clang's default output loads and its exceptions work (40 / 42 / 10 / 10 / −1) |
| `T20` Dart | still yellow — the recovery accepts the encoding, then the `canonical type reference is not declared by this module` failure (`F3`-adjacent, still unreproduced) |
| Final-form modules (Kotlin, Go, Rust, Zig, C, MoonBit) | byte-identical; `[legacy-eh]` asserts no Note is emitted for Binaryen's `try_catch.exnref` twin |

## 6. What changed in the report set

- [`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md) §3.5 records the default-flags transcript
  and calls the flag a preference; `T05` moved from yellow to **green**.
- [`LF2_S4_WASMGC.md`](LF2_S4_WASMGC.md) §2 records that Kotlin/Wasm 2.4.21
  emits `try_table`, correcting the `LF-1` premise that it depends on legacy EH.
- [`LF2_INDEX.md`](LF2_INDEX.md) §4/§5 marks `F2` fixed and `T05` green;
  [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §5.2 keeps the "before" transcript and
  adds the "after" one.

## 7. What this note does not claim

- That every legacy-EH producer emits only shapes the rewrite accepts. Lua's and
  clang's do; Dart's is untested because a different failure (its canonical type
  reference) stops it first.
- That `wasm-opt --translate-to-exnref` and the rewrite agree on every module.
  One hand-built twin disagreed with the rewrite before the escape block existed
  and was not re-run (§4.1); clang's real 336 KB module and the four fixtures are
  the evidence, not a proof for all inputs.
- That the escape block is free. It costs a `block` around each skipped frame, a
  `catch_all_ref` at the delegated body and one `exnref` local per function; the
  measured load-time effect of the pass itself is in §5.1.
