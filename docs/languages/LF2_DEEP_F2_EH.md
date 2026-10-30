# `F2` — Legacy `delegate` (`LE-8`): Workaround and Long-Term Options {#lf2_deep_f2}

Status: deep-study note (revision 1 — 2026-10-30). It investigates `F2` of
[`LF2_INDEX.md`](LF2_INDEX.md) §5: a default `clang++` C++ module does not load
because its legacy exception encoding contains the one shape
`ParseOptions::legacyExceptionHandling` refuses (`LE-8` of
[`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6). It
verifies the `wasm-opt --translate-to-exnref` workaround end to end, states the
toolchain flags that avoid the shape, and analyses what a longer-term in-engine
solution would actually have to do.

Audience: whoever owns `WasmBase/WasmLegacyEh.cpp`, whoever writes the C/C++ or
Kotlin embedding recipe, and `LF-3` when it prioritises the EH work.

**Headline.** The workaround works (translated module loads, constructor runs,
guest `try`/`catch` returns 42/−1). It is a *build-time or one-time* fix, not an
in-engine one. The engine's rewrite cannot express this shape without a
filtering mechanism, so the honest long-term choice is "final EH is the
supported encoding; legacy EH is a best-effort convenience", with an in-engine
translation pass as a separate, larger milestone if a consumer appears.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What the legacy construct is, and why the rewrite refuses it |
| §3 | The workarounds, verified |
| §4 | Why "just extend the rewrite" is not a small change |
| §5 | The long-term options and the recommendation |
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

That refusal is **sound**. A plain block has no effect on exception routing, so
the intervening `try_table` — which the rewrite produced for the middle try's
arms — would catch an exception the original module says must skip it. Silently
accepting the module would change program semantics; refusing by name is the
correct behaviour of a best-effort rewrite.

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

`T05` stays **yellow**: the bar passes with a flag, and the flag is a toolchain
configuration, not an engine patch. The verdict no longer says "requires engine
work"; it says "requires the final-EH flag, or a one-time `wasm-opt` pass".

## 4. Why "just extend the rewrite" is not a small change

The obvious ask — "make the rewrite handle `delegate` that skips a catch" — is
not a local patch, because the rewritten form has no way to say *skip*:

- A legacy `try … delegate N` becomes a `block`, which routes exceptions to the
  enclosing `try_table`s. There is no "propagate past this handler" bit.
- The handler the exception must skip is a `try_table` clause. A raised
  exception is matched by the innermost clause whose tag matches; a catch-all
  matches everything. Making the intervening clause skip one region therefore
  requires either
  (a) **structural hoisting** — moving the delegated body out of the
  intervening `try_table`'s syntactic extent. That is a function-wide
  restructuring (operands, locals, control nesting, branch depths), not a
  peephole rewrite; or
  (b) **filtering** — catching everything and rethrowing unless the exception
  originated in a delegated region, which needs a per-exception identity the
  legacy proposal never had. `wasm-opt`'s translation achieves the same result
  by rewriting the *whole* function into `exnref` form, where the exception
  value can be carried and rethrown (`catch_all_ref` + `throw_ref`) and the
  nesting is rearranged accordingly.

So the choice is between "a translation pass of the same class as
`wasm-opt --translate-to-exnref`" and "do not support this shape". The current
rewrite is a convenience for shapes that *can* be lowered in place — Lua's
`setjmp` output is one, and it is accepted
([`LF2_S5_GUEST_VMS.md`](LF2_S5_GUEST_VMS.md) §2).

### 4.1 A cross-check, and why the refusal stays

A minimal `try (try (throw) delegate 1) catch $t` module was run two ways: through
our rewrite (`WASMJIT_LEGACY_EH=1`) and through `wasm-opt --all-features
--translate-to-exnref`. The rewrite **accepted** the shape and returned 99 (the
outer catch handled it); the Binaryen translation produced a
"caller-target" `exnref` block whose execution through the engine returned 0.
The two disagree, and the Binaryen output is not obviously executable as a plain
module here, so the cross-check is **inconclusive** — it neither confirms nor
refutes the frame accounting.

What it does establish is that the refusal is **narrow**: it fires only when the
rewrite's own frame stack says a catch-bearing legacy `try` sits between the
delegated body and its target. Clang's `-O2` module has exactly that nesting
(§2), and the engine keeps refusing it by name. Turning an unverified
reconstruction into accepted semantics would be worse than a yellow verdict, so
`T05` keeps yellow and the flag/`wasm-opt` path stays the supported one until
someone builds the translation pass.

## 5. The long-term options

| Option | Shape of the work | Class | Verdict |
|---|---|---|---|
| **A. Final EH is the supported encoding** — toolchain flags for source builds, `wasm-opt` for prebuilt modules, the rewrite stays best-effort | documentation + the error text already names both fixes | **D** (a recorded policy decision) | **recommended** |
| **B. In-engine translation pass** — a `--translate-to-exnref` equivalent inside the loader, producing `exnref` + `try_table` with function-wide restructuring | a real pass over every EH-bearing function; testable against the same modules | **A**, but a milestone of its own | open — only worth it if a consumer that cannot re-flag appears |
| **C. Extend the rewrite with `delegate`-skip** | impossible in place (§4); it would become B | — | not viable as stated |
| **D. Runtime support for the legacy encoding** | the legacy instructions are superseded; the validator and every backend would need them | **C/E** | rejected |

**Why A is the recommendation, not a compromise.** Every toolchain tested in
`LF-2` that emits exceptions can emit the final encoding today: clang (one
`-mllvm` flag), Emscripten (one `-s` flag), Kotlin/Wasm (it does by default).
The only producer that *cannot* re-flag is a prebuilt module, and for that
`wasm-opt` is a one-time, offline step whose output is a normal `try_table`
module. A rewriting pass inside the engine would add a second, subtly different
EH implementation to maintain, for a format the ecosystem is leaving.

**What is still owed.** The refusal message must keep naming **both** fixes
(the flag and the `wasm-opt` command) — it does — and the C/C++ embedding
recipe must stop presenting "no flag" as a normal build. That is a docs change,
and `S1` carries it.

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
| C++ 20 / clang default | yes, **and** the `delegate`-skip shape | no — `LE-8` is refused with the rewrite on *or* off |
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
*can* handle, and leaves the `LE-8`/Dart cases failing with their own, accurate
errors.

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
| `T05` C++ default flags | unchanged yellow — now fails with `LE-8`'s message (the rewrite is on, so the remedy it prints is the rebuild/`wasm-opt` one, not `WASMJIT_LEGACY_EH`) |
| `T20` Dart | unchanged yellow — the recovery accepts the encoding, then the `canonical type reference is not declared by this module` failure (`F3`-adjacent, still unreproduced) |
| Final-form modules (Kotlin, Go, Rust, Zig, C, MoonBit) | byte-identical; `[legacy-eh]` asserts no Note is emitted for Binaryen's `try_catch.exnref` twin |

## 6. What changed in the report set

- [`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md) §3.5 now names
  `-sWASM_LEGACY_EXCEPTIONS=0` / `wasm-opt` beside the `clang` flag.
- [`LF2_S4_WASMGC.md`](LF2_S4_WASMGC.md) §2 records that Kotlin/Wasm 2.4.21
  emits `try_table`, correcting the `LF-1` premise that it depends on legacy EH.
- `T05` keeps its yellow verdict, for the flag rather than for "engine work".

## 7. What this note does not claim

- That every legacy-EH producer emits only shapes the rewrite accepts. Lua's
  does; Dart's is untested because `F3` stops it earlier.
- That `wasm-opt --translate-to-exnref` is semantics-preserving for every
  module. It was verified on one module (`t05_eh.wasm`) with one transcript;
  that is the evidence, not a proof.
- That option B is small. It is a function-level restructuring pass; the note
  prices it as a milestone so that nobody budgets it as a parser tweak.
