# `LF-2` Evidence — Toolchains, Modules and Findings {#lf2_evidence}

Status: evidence log for the `LF-2` report set (revision 1 — 2026-10-30). Every
verdict in this folder names a row here: the toolchain with its version and
flags, the module with its SHA-256, the inspection command, and the transcript.
The discipline is [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.4:
*a finding without a reproducible artefact is an opinion.*

Audience: anyone re-running an arm, and whoever picks up `F1`–`F3` in `LF-3`.

**What was actually done.** Every toolchain in §2 was installed in a study
workspace under `build/lf2/toolchains/`, every module in §3 was compiled there
and inspected with `wasm-tools` 1.252.0, and every transcript in §4 is from the
runner in §6. Nothing was patched, and no file outside `build/` and `docs/`
changed.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which toolchains, which versions, which flags? |
| §3 | The modules: hash, size, imports, exports |
| §4 | The transcripts |
| §5 | The three findings, with reproduction steps |
| §6 | The runner, and how to rebuild it |
| §7 | What was not verified |

## 2. Toolchains

| Toolchain | Version | Install | Flags used |
|---|---|---|---|
| clang (system) | 23.1.4 | present | `--target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--allow-undefined` |
| wasi-sdk | 34.0 (`clang version 23.1.0-wasi-sdk`) | release tarball, 192 MB | `--target=wasm32-wasip1 -O2 -mexec-model=reactor`; C++ adds `-fwasm-exceptions [-mllvm -wasm-use-legacy-eh=false] -lc++ -lc++abi -lunwind -lsetjmp` |
| Emscripten | 6.0.13 | `emsdk install latest` | `-O0`/`-O1`/`-O2 -sSTANDALONE_WASM=1 [--no-entry] -sEXPORTED_FUNCTIONS=…` |
| rustc | 1.99.1 (2026-10-13) | `rustup` + `target add wasm32-unknown-unknown wasm32-wasip1` | `-O --crate-type=cdylib` |
| Go | 1.27.3 | present | `GOOS=wasip1 GOARCH=wasm go build [-buildmode=c-shared]` |
| Go (for TinyGo) | 1.25.6 | release tarball | used only as TinyGo's front end |
| TinyGo | 0.40.1 (LLVM 20.1.1) | present | `-target=wasi -opt=0` |
| Zig | 0.15.2 | present | `build-exe -target wasm32-freestanding -O ReleaseSmall -fno-entry -rdynamic` |
| AssemblyScript | 0.28.19 | `Test/node_modules` | `--optimize --runtime stub --noAssert --initialMemory 1` |
| Dart | 3.13.7 | SDK zip | `dart compile wasm --standalone` |
| Kotlin | 2.4.21 + Gradle 9.1.0 (JDK 25) | compiler zip + Gradle distribution | `wasmJs { nodejs() }`, `wasmWasi` |
| Lua | 5.4.8 | source tarball | see [`LF2_S5_GUEST_VMS.md`](LF2_S5_GUEST_VMS.md) §2 |
| wasm-tools | 1.252.0 | present | `print`, `validate --features all`, `parse` |

Two installs are worth recording because they are not obvious: rustc's wasm
targets are **not** part of a distribution rustc (the Gentoo package ships
`x86_64`/`i686` only), so `rustup target add` was required; and `emsdk` writes
absolute paths into its config, so the workspace must not move between
`install` and `activate`.

## 3. The modules

All under `build/lf2/poc/modules/`; hashes are SHA-256.

| Module | Row | Bytes | Imports | Exports | SHA-256 |
|---|---|---:|---:|---:|---|
| `t02_answer.wasm` | `T02` | 468 | 1 | 7 | `e6e3a660fc85865281169e02d9da386061b5dc32ebcef76b3370e52003a6358c` |
| `t02_dep.wasm` | `T02` | 354 | 0 | 2 | `aaf7e4ebd51a2c6293a56cb6a0807559e87113877edc7530505321ef3929a7a6` |
| `t02_caller.wasm` | `T02` | 357 | 1 | 1 | `1f4c0a3885b5e00cc51e592026f8d135cedb30036e3be9cf44e80f6c2afd79a6` |
| `t03_reactor.wasm` | `T03` | 60,189 | 4 | 7 | `d1db310b419ee7697d2b92b39c42cce392be09ccccbe7366c04fd4ad1acc73fc` |
| `t01_emcc_o1.wasm` | `T01` | 1,742 | 3 | 9 | `901dc4aad242b7f9ede054e95e9be028f7fbe47dc06effa94550194bc3f9ac11` |
| `t01_emcc.wasm` (O2) | `T01` | 1,742 | 3 | 9 | refused (`F1`) |
| `t04_empp_reactor.wasm` | `T04` | 11,613 | 2 | 9 | `3ff1feea0e8150dd88a6c8a688a33011f5ce70cab6306c298f0b196d47921953` |
| `t05_final_eh.wasm` | `T05` | 336,339 | 7 | 7 | `dad7365286cba0af63c4ccb1a3b3bbc47dfa5978510033fded6568146635c919` |
| `t05_eh.wasm` (default) | `T05` | 336,269 | 7 | 7 | `31f99abb3201f0fecc9331376be094c83416aaf0b2cac6e10e3dbf281e1eee95` (was refused, `F2`; runs now) |
| `t06_rust.wasm` | `T06` | 1,653,586 | 1 | 6 | `1d6f359b36a9b04bb1e1639961fb2d3d40a5de101b62ac8b87437acb230364ae` |
| `t07_rust.wasm` | `T07` | 2,167,021 | 5 | 4 | `3878f95acaeba22d371501346226a40a9eee2cad770e634f40b617c211b99878` |
| `t09_go_lib.wasm` | `T09` | 2,479,821 | 18 | 5 | `98af3f7b4ae0d92eee1cb04eec215ff54dd6e64789037862e4ac8e3211d42c30` |
| `t09_go.wasm` | `T09` | 2,477,280 | 16 | 2 | `7310ddb57bcd3b3108a4fc98502479c455dddb721e888ba1060fd77717c30690` |
| `t10_tinygo.wasm` | `T10` | 1,571,287 | 7 | 13 | `2218150763fa0a711f713533c8e26dc7ca22da16cffb4b5e7c46ac5b17257646` |
| `t33_zig.wasm` | `T33` | 108 | 0 | 4 | `1d14fd418c10f3d576279fe6f12ce4cd155a62f17c98badf45dbd6f38427462f` |
| `t15_as.wasm` | `T15` | 144 | 1 | 5 | `45b0373db2ec315a4b71706db5c81cc3409e24a9d217dc6de8e1d5e994d85bf5` |
| `t26_lua.wasm` | `T26` | 602,204 | 18 | 6 | `a266edd5961bd3ab90d6ec08d0711518b88f652b1f0bbb48e484ea1810a5e88a` |
| `t53_kotlin_wasi.wasm` | `T53` | 614,799 | 3 | 5 | `e466e0abc1cb3537a34b654e8c01e70e1268b0be1448b74dd35c25bc7a604da6` (refused, `F3`) |
| `t53_kotlin.wasm` (`wasmJs`) | `T53` | 620,331 | 696 | 4 | `7431cde91542c918dd0a67b26b3494704cb4e629a8eb3eff8e22b1ebae3b8a52` |
| `t20_dart.wasm` | `T20` | 15,768 | 10 | 1 | `b57d7da6c748f952b08c69ab7fccc9b666ecedc24cad148bc51a8ae65cfad3ff` (refused, `F3`) |
| `gc_bottom_subtype.wasm` | `F3` | 118 | 0 | 1 | `9303e6b01d3dbb22b9f9313b0c09eb4345340693d445a6e61a41d52e55fb81cf` |
| `backend_ops.wasm` | control | 175 | 0 | 3 | `47b28619ed8d1f5ea4d94a3283fc16dec9a823ae142e1aafd088cad7c26219e7` |
| `t86_moonbit.wasm` | `T86` | 359 | 1 | 1 | `0dd87c96e040d04bdd1c245ebe60028490a1fc948130d7f78f14b2ad85976114` |

`backend_ops.wasm` is the negative control for `F1`: it exercises `call_indirect`,
`memory.copy` and `memory.fill` in isolation and **all three run**, so the
failure is not "the backend lacks a bulk-memory opcode".

The module sources live in `build/lf2/poc/src/<toolchain>/`; each arm's build
command is in its cluster report. The workspace is a study artefact and is not
checked in — the hashes plus the commands are what makes a rebuild verifiable.

## 4. The transcripts

The full transcript of each arm is in its cluster report (§3 of each). The four
that carry a finding are reproduced here verbatim, because they are the evidence
a fix would be tested against.

```
# F1 — Emscripten -O2, instantiation
jit_function_compile failed 0 for func 2
-- Dumping function before abort --
function 0x562EFB1FF240(l214 : ptr) : void
	incoming_reg(l214, rdi)
.L:
	i216 = 0
	i218 = 0
	outgoing_reg(l214, rdi)
[std::abort]
```

```
# F2 — C++ default flags, load
status=error: legacy exception handling (the superseded Phase-3 proposal) is not
supported: try (0x06). … or set WASMJIT_LEGACY_EH=1 to load it through the
built-in rewrite.
```

> Recorded under the pre-`LE-6` strict default. The default is now the recovery
> policy, so the same module reports
> `legacy exception handling: this module uses a construct the built-in rewrite
> does not translate (delegate that skips an intervening catch)` — the transcript
> below — and the named refusal above is what `WASMJIT_LEGACY_EH=0` still prints.

```
# F2 — the same module with the rewrite enabled
status=error: legacy exception handling: this module uses a construct the
built-in rewrite does not translate (delegate that skips an intervening catch).
```

```
# F3 — a valid GC module, load
status=error: type section: invalid `sub` annotation (comptype does not match
its supertype)
```

The positive transcripts — `T02`'s cross-module call, `T03`'s `printf`, `T05`'s
guest `try`/`catch`, `T09`'s `_initialize` then export, `T26`'s script calling a
host import — are quoted in the cluster reports and are not repeated here.

## 5. The findings

### 5.1 `F1` — the `-O2` Emscripten abort (class B)

**Observe.** `emcc -O2` produces a module that aborts during instantiation, in
`LibjitModuleCompiler.cpp:234`, while compiling the fifth defined body
(`_start`). `emcc -O1` and `-O0` of the same source load and run.

**Reproduce.** Build `poc/src/c/em_main.c` at `-O2` and at `-O1`; run both
through the runner. `t01_emcc_o1.wasm` prints the Emscripten `hello`; the `-O2`
module aborts before a single export is callable.

**Bound the claim.** It is not bulk memory (`backend_ops.wasm` passes), not the
Emscripten import set (`T01` at `-O1` passes), and not "the backend cannot
compile optimised code" (`T06`, `T07`, `T10`, `T33` are all optimised builds
that pass). It is one function in one optimisation configuration. The internal
cause is `LF-3` work; this study does not diagnose it.

### 5.2 `F2` — `LE-8` in default C++ output (class A, **fixed**)

**Observe.** `clang++ -fwasm-exceptions` (LLVM 23.1.x, wasi-sdk 34.0) still
emits the legacy encoding by default. Before the fix the rewrite refused the
module *with the rewrite on* (`WASMJIT_LEGACY_EH=1`, and equally under the
default recovery policy), with the `LE-8` message; adding
`-mllvm -wasm-use-legacy-eh=false` produced a `try_table` module that loaded and
ran, guest exceptions included.

**Reproduce.** `poc/src/cpp/hello.cpp`, two builds as in
[`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md) §3.5.

**After the fix** (`LE-8`, escape block; same binary, same hash):

```
call _initialize/void() -> void
call ctor_value/i32() -> i32=40
call via_host/i32(20,22) -> i32=42
call sum_vector/i32(4) -> i32=10
call throw_catch/i32(5) -> i32=10
call throw_catch/i32(-3) -> i32=-1
status=ok
```

The load reports `WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN` under the default
policy; `WASMJIT_LEGACY_EH=1` gives the same result without the note.

**Bound the claim.** This study supplied a real consumer (libc++'s `__cxa_throw`
path) for a row of
[`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6 that
had only hand-written fixtures. The translation is exercised by four fixtures
(`delegate_skip` ×2, `delegate_skip_table`, `d1`/`d2`) and by this module's own
cleanup pads; it is not a proof for every producer's output.

### 5.3 `F3` — bottom heap types are not subtypes (class A)

**Observe.** `gc_bottom_subtype.wasm` (§3) is valid per `wasm-tools` and is
refused by the type registry.

**Cause, at the code level.** `heapMatches` in `WasmBase/WasmTypeRegistry.cpp`
handles abstract/abstract, concrete/abstract and concrete/concrete. The legal
case *abstract-bottom / concrete* (`none`, `nofunc`, `noextern` against a type
index) falls through to `false`, so any `sub` whose field narrows a reference to
a bottom type is rejected. `compMatches` calls it for every field of every
declared supertype.

**Bound the claim.** The five-line module proves the rule is missing. It
explains the Dart module exactly (type 41 narrows `(ref null 1)` to `nullref`).
It is **not** established that Kotlin's module fails for the same reason — its
`sub` annotations contain no `nullref` — so the fix's test must use both modules
and `LF-3` should not close the finding on the repro alone.

## 6. The runner, and how to rebuild it

The runner is `build/lf2/poc/probe.cpp`, compiled against `build/LF2`:

```
g++ -std=gnu++2a -O2 -I<repo> -I<repo>/extern/MhLib -I/usr/include/nspr \
    -I/usr/include/freetype2 probe.cpp -o probe \
    -L<build>/WasmBase -lWasmBase -L<build>/LibJit -lLibJit \
    -L<build>/extern/MhLib/Euphemy -lEuphemy \
    -L<build>/extern/MhLib/Elvavena -lElvavena -ljit -lffi
```

It is deliberately thin: the §6.4 recipe (`Runtime::create` → `loadFile` →
`instantiate` → `callExport`), a `RegistryImportResolver`, 20 WASI entry points
registered through `WASM::nativeImport<&fn>()`, two `env` imports, and a
`--link dep.wasm:module:export` mode for the cross-module bar. There is no
alternate execution path and no engine hook it invents — which is what
[`CHARTERS.md`](CHARTERS.md) §2.2 means by "the documented recipe, never a
bespoke harness".

The shadow build itself is the documented one:

```
mkdir build/LF2 && cd build/LF2
qmake6 ../../Project.pro            # see the argv[0] note below
env MAKE=/usr/bin/make /usr/bin/make -j16
```

Two operational notes, both environmental rather than engine-related: this shell
rewrites `argv[0]`, so compilers and `qmake` had to be started with `exec -a`
(the same quirk the repository's `AGENTS.md` records for `make`); and the
`Makefile` must be generated *after* that workaround, or it records the wrong
`QMAKE` and every sub-project fails with "No rule to make target 'Makefile'".

## 7. What was not verified

| Not done | Consequence |
|---|---|
| A reduction of `F1` to the failing instruction | `F1` names the function and the flag boundary, not the cause. `LF-3` owns the reduction. |
| The Kotlin/Dart type-section mismatch, item by item | `F3`'s rule is proven; that both modules fail *for that rule* is not. |
| Cross-module linking for every arm | proven once (`T02`); the mechanism is the engine's, and no other arm needed a second copy. |
| String/aggregate marshalling through the runner | it has no string argument mode; the AssemblyScript harness covers that direction, and no other arm needed it to pass the bar. |
| Performance, memory ceilings, start-up time | out of scope ([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §10). |
| The 83 rows listed in [`LF2_L6_L12.md`](LF2_L6_L12.md) | no toolchain was installed for them; their `LF-1` dispositions stand. |
| Any product change | none was made: no `WasmBase/`, backend, header or build file changed, and `git status` shows only `docs/` (plus the ignored `build/` workspace). |

## 8. After the fixes: re-run evidence

The three deep notes end in code, so this log records what the engine does
**after** the fixes and what a re-run sees. Nothing here changes the `LF-1`
screen; it is the `LF-2` set's own delta.

| Artefact | What it shows |
|---|---|
| `build/lf2/poc/f1_libjit_probe.c` | libjit compiles the failing CFG at `opt_level=0` (`compile=1`) and fails at level 1 (`compile=0`); a retry after failure segfaults |
| `build/lf2/poc/modules/f1_reduced.wat` | the exact `_start` shape of an `emcc -O2` module, reduced |
| `Test/wasm_wat/bottom_subtype.wat` | the `F3` repro, now a permanent fixture |
| `t01_emcc.wasm` (unchanged hash) | instantiates and runs at `-O2`; `answer`/`add`/`via_host` = 42, `_start` → `wasi_exit=0` |
| `t04_empp_reactor.wasm` (unchanged hash) | `-O2` loads; ctors and `std::vector` work |
| `t53_kotlin_wasi.wasm` (unchanged hash) | runs: `add`/`answer`/`throwCatch` and `_start`; the `F3d` ownership fix is what unblocked it |
| `t09_go_lib.wasm` (rebuilt) | `_initialize`, `env.host_add` = 42 and a cross-module `dep.add` = 43 through `//go:wasmimport` |
| `t20_dart.wasm` (unchanged hash) | gets past the type section; fails on legacy EH (`F2`), and under the recovery policy (or `WASMJIT_LEGACY_EH=1`) on `canonical type reference is not declared by this module` |
| `WASMJIT_LIBJIT_OPT=1` | reproduces `F1` as a reported failure (`backend compile failure … result 0 for function 7`), with no `SIGABRT`/`SIGSEGV` |

The module hashes are unchanged: the fixes are in the engine, not in the
toolchains, so a re-run compares like with like.

## 9. References

- [`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.4/§8.5 — the
  evidence a verdict must carry, and the verdict rules.
- [`LF2_INDEX.md`](LF2_INDEX.md) — the verdict table these rows feed.
- [`LF2_DEEP_F1_BACKEND.md`](LF2_DEEP_F1_BACKEND.md),
  [`LF2_DEEP_F2_EH.md`](LF2_DEEP_F2_EH.md),
  [`LF2_DEEP_F3_GC.md`](LF2_DEEP_F3_GC.md) — the deep studies.
- [`ABI.md`](../ABI.md), [`EMBEDDING_SURFACE.md`](../EMBEDDING_SURFACE.md) — the
  ABI and the recipe the runner implements.
- [`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6 —
  the `LE-1`…`LE-9` rows `F2` lands in.
- [`DIAGNOSTICS.md`](../DIAGNOSTICS.md) §5.1/§5.3 — the new id and the host
  knobs.
- [`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 — the classes `F1`–`F3`
  are assigned.
