# `LF-4` Evidence {#lf4_evidence}

*Toolchains, Components and the Reproduction*

Status: evidence log for the `LF-4` report set (revision 1 — 2026-11-16). Every
verdict names a row here: the toolchain with its version and flags, the component
with its SHA-256, the inspection command, and the transcript. The discipline is
[`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.4 — *a finding
without a reproducible artefact is an opinion*.

**What was actually done.** Seven components were built in a study workspace
under `build/cm/` with the upstream guide's own steps, inspected with
`wasm-tools` 1.252.0, and run through the component embedding recipe. Nothing was
patched: no `WasmBase/`, backend, header, `.pro` or product dependency changed,
and `git status` shows only `docs/`. Tool installation directories are
deliberately **not** recorded; versions, flags and artifact hashes are.

Audience: anyone re-running an arm, and whoever picks up `GAP-20` in `LF-3`.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which toolchains, which versions, which flags? |
| §3 | The artifacts: size, hash, imports, export |
| §4 | The transcripts |
| §5 | The finding, with its reproduction |
| §6 | The runner, and how to rebuild it |
| §7 | What wasi-sdk 34.0 consists of |
| §8 | What was not verified |
| §9 | References |

## 2. Toolchains

| Toolchain | Version | Install | Flags or commands used |
|---|---|---|---|
| wasi-sdk | 34.0 (`clang version 23.1.0-wasi-sdk`, default target `wasm32-unknown-wasip1`) | release tarball (its installation directory is deliberately not recorded; its **contents** are in §7) | `wasm32-wasip2-clang -mexec-model=reactor -O2`; C++ adds `wasm32-wasip2-clang++` |
| wit-bindgen CLI | 0.63.0 | present | `c`, `cpp` and `moonbit` generators |
| wasm-tools | 1.252.0 | present | `component wit`, `component embed`, `component new`, `validate --features all`, `print` |
| .NET SDK | 10.0.113 | present | `dotnet new install BytecodeAlliance.Componentize.DotNet.Templates`; `dotnet new componentize.wasi.cli -o adder`; `dotnet build -c Release` |
| componentize-dotnet | `0.8.0-preview*` (templates + Wasm SDK), wit-bindgen 0.58.0, `runtime.linux-x64.microsoft.dotnet.ilcompiler.llvm` 10.0.0-rc.1.26306.1 | NuGet, restored by the build | no hand-written `Publish`/`wac` step: the SDK emits the component |
| rustc / cargo | 1.99.1 (2026-10-13), `wasm32-wasip2` std | rustup toolchain + `target add wasm32-wasip2` | `cargo build --target wasm32-wasip2 --release`, `wit-bindgen` crate 0.63.0 |
| Go | 1.27.4 | present | `componentize-go` 0.4.2 (installed from its module), `go mod tidy` |
| componentize-go | 0.4.2, wit-bindgen 0.61.1, Go 1.25 | `go install`; the CLI then downloads its own release binary on first run | `componentize-go --world adder bindings`; `componentize-go build` |
| TinyGo | 0.40.1 (LLVM 20.1.1) | present | `-target=wasip2 -no-debug --wit-package docs:adder@0.1.0.wasm --wit-world adder` |
| wit-bindgen-go | `go.bytecodealliance.org` v0.7.0 (`go tool`) | `go get -tool` | `wit-bindgen-go generate --world adder --out internal` |
| MoonBit | moon 0.1.20261028 (e1e1f15), moonc/moonrun of the same build | present | `wit-bindgen moonbit … --derive-eq --derive-show --derive-error`; `moon build --release` |

Two version notes that matter for a re-run:

- **TinyGo 0.40.1 refuses a Go 1.27 front end** (“requires go version 1.19
  through 1.25, got go1.27”). The arm was built with `GOTOOLCHAIN=go1.25.6`, and
  the `go.mod` that `wit-bindgen-go` generated under 1.27 had to have its `go`
  directive lowered from `1.27.4` to `1.25.6` before TinyGo would compile it.
- **.NET's componentize path is not the guide's exact source layout any more.**
  The generated interface/impl names in wit-bindgen 0.58.0 are
  `IAddExports`/`AddExportsImpl` (the guide shows `IAdd`/`AddImpl`) and the
  `Wit` item must name `World="example"`. The source was adjusted to the
  generated names; see [`LF4_S1_DOTNET.md`](LF4_S1_DOTNET.md) §3.

## 3. The artifacts

All under `build/cm/arms/<arm>/`; sizes in bytes, hashes SHA-256. “Imports”
counts the component's imported *interfaces*, not core functions.

| Arm | Artifact | Bytes | Imports | Export | SHA-256 |
|---|---|---:|---:|---|---|
| `s2-c` | `adder.wasm` | 50,626 | 0 | `docs:adder/add@0.1.0` | `96d7513337d3c97bb2689d763e38cb9ea7a199690788f7c17c81bf5cb7a0dfd1` |
| `s2-cpp` | `adder.wasm` | 50,670 | 0 | `docs:adder/add@0.1.0` | `0d3e5bd137189bd1389c0b0f0c7dcd765b81b5d11f307b4cd369259203673ab7` |
| `s2-c-libc` | `adder.wasm` | 274,861 | 13 (`wasi:io`, `wasi:cli`, `wasi:clocks`, WASI 0.2.12) | `docs:adder/add@0.1.0` | `6c517e1f6f98ab61dc6b28adb3f289e154a1b66615e966f67454187847b23840` |
| `s4-rust` | `target/wasm32-wasip2/release/adder.wasm` | 13,719 | 0 | `docs:adder/add@0.1.0` | `94b05bf7e2c721a3d8456565419efdcd53a80c77cd99a43db6c1ac8b5bcd8e96` |
| `s5-moonbit` | `adder.component.wasm` | 3,236 | 0 | `docs:adder/add@0.1.0` | `662c80a97a43e6b44bd0ce5cd3cca3e2e8f3d48ce89b37a79777344afe5e176f` |
| `s1-dotnet` | `adder/bin/Release/net10.0/wasi-wasm/native/adder.wasm` | 2,227,102 | 18 (WASI 0.2.6) | `docs:adder/add@0.1.0` | `9e60c4b176ce290a4e0f186c0da3573da910b862d24cc35cd47d604eb9b03938` |
| `s3-go` | `main.wasm` | 2,524,500 | 18 (WASI 0.2.12) | `docs:adder/add@0.1.0` | `d53945a962544106113c411e27b527c1d04252fa4912a5131dac406912621d4e` |
| `s3-tinygo` | `adder.wasm` | 22,207 | 4 (`wasi:io/error`, `wasi:io/streams`, `wasi:cli/stdout`, `wasi:random/random`, WASI 0.2.0) | `docs:adder/add@0.1.0` | `db22255abdcb6c9ea502e8a9ffc191102bfd49b8ff39bb1132b6a60e6520cb2c` |
| `u64-probe` | `probe.wasm` | 50,644 | 0 | `docs:u64probe/get@0.1.0` | `a65695141bc37e522487450ae28eeb871255b23ca998a02eb67b9cc9f65e64f5` |

Every artifact passes `wasm-tools validate --features all`; the four 🟩 Green

## 4. The transcripts

Each transcript is `component_probe <artifact>` with the arguments named. The
lines the loader emits first are identical for every arm that reaches
instantiation: the artifact path, the backend, the size and the feature set.

### 4.1 `s2-c` — C, wasi-sdk, no libc (🟩 Green)

```
loaded path=…/s2-c/adder.wasm backend=libjit size=50626
features=baseline,values,async,threads,component-imports,memory64,error-context
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

The component's `wasm-tools component wit` is exactly the `adder` world with no
imports. 50 KB is almost entirely the `wit-bindgen` canonical-ABI glue and
wasi-libc's `cabi_realloc`; nothing is imported.

### 4.2 `s2-cpp` — C++, wasi-sdk, `wit-bindgen cpp` (🟩 Green)

```
loaded … size=50670
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

The same shape as the C arm. `wit-bindgen cpp` emits a namespace
(`exports::docs::adder::add`) and the implementation is a plain `uint32_t Add`.

### 4.3 `s2-c-libc` — C, wasi-sdk, with `printf` (🟨 Yellow)

```
status=error: component validation: canonical option `memory` is required
```

The component is 274,861 bytes and imports 13 WASI 0.2.12 interfaces
(`wasi:io/error`, `wasi:io/poll`, `wasi:io/streams`, `wasi:cli/stdin|stdout|stderr`,
`wasi:cli/terminal-*`, `wasi:clocks/monotonic-clock|wall-clock`). The engine
rejects it **at load**, before instantiation, so no host import is ever looked
up. `wasm-tools validate --features all` accepts it. `GAP-20`
([`LF4_INDEX.md`](LF4_INDEX.md) §5).

### 4.4 `s4-rust` — Rust, `wasm32-wasip2` (🟩 Green)

```
loaded … size=13719
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

`wasm-tools component wit`:

```
package root:component;
world root { export docs:adder/add@0.1.0; }
package docs:adder@0.1.0 { interface add { add: func(x: u32, y: u32) -> u32; } }
```

Rust's `wasm32-wasip2` target emits a component directly, and a component that
imports nothing from WASI imports nothing at all.

### 4.5 `s5-moonbit` — MoonBit (🟩 Green)

```
loaded … size=3236
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

A 3,236-byte component: `moon build --release` emits a core module,
`wasm-tools component embed` attaches the WIT and `wasm-tools component new`
lifts it. No imports.

### 4.6 `s1-dotnet` — C# (🟨 Yellow)

```
status=error: component validation: canonical option `memory` is required
```

2,227,102 bytes, 18 WASI 0.2.6 imports, one `canon lift` for
`docs:adder/add@0.1.0#add`, valid per `wasm-tools`. `GAP-20`.

### 4.7 `s3-go` — Go, `componentize-go` (🟨 Yellow)

```
status=error: component validation: canonical option `memory` is required
```

2,524,500 bytes, 18 WASI 0.2.12 imports. `GAP-20`.

### 4.8 `s3-tinygo` — TinyGo, `-target=wasip2` (🟨 Yellow)

```
status=error: component validation: canonical option `memory` is required
```

22,207 bytes, 4 WASI 0.2.0 imports (`wasi:io/error`, `wasi:io/streams`,
`wasi:cli/stdout`, `wasi:random/random`). `GAP-20`.

## 5. The finding and its reproduction

`GAP-20` is [`LF4_INDEX.md`](LF4_INDEX.md) §5. Its minimal reproduction is the
`u64-probe` row above:

1. Write a WIT package whose single interface is `get: func() -> u64` and whose
   world exports it.
2. `wit-bindgen c probe.wit --out-dir gen`, implement
   `exports_docs_u64probe_get_get`, and link with
   `wasm32-wasip2-clang -mexec-model=reactor -O2`.
3. `wasm-tools validate --features all probe.wasm` → exit 0.
4. `component_probe probe.wasm` → `status=error: component validation:
   canonical option `memory` is required`.

The printed component contains exactly one canon definition, a bare
`(canon lift (core func $docs:u64probe/get@0.1.0#get))`, so the missing `memory`
option the validator asks for is not missing at all: the ABI does not require
one for a single `i64` result.


## 6. The runner, and how to rebuild it

The runner is `build/cm/poc/component_probe.cpp`, a study artifact (not product
code). It is thin on purpose: it uses only `Runtime::create`,
`Runtime::loadComponentFile`, `Runtime::instantiateComponent`,
`ComponentInstanceHandle::exportedInstance`/`exportedFunction` and
`ComponentFunction::callValues`, with `--bag`, `--bagcall bag/name:args` and
`--call name:args` for the cases it needs. There is no alternate execution path
and no engine hook it invents.

Compile line (against the documented shadow build; `exec -a` is the `argv[0]`
workaround `AGENTS.md` records):

```
exec -a g++ /usr/bin/g++ -std=gnu++2a -O2 \
    -I<repo> -I<repo>/extern/MhLib \
    component_probe.cpp -o component_probe \
    -L<build>/WasmBase -lWasmBase -L<build>/LibJit -lLibJit \
    -L<build>/extern/MhLib/Euphemy -lEuphemy \
    -L<build>/extern/MhLib/Elvavena -lElvavena -ljit -lffi
```

Run it with `LD_LIBRARY_PATH` pointing at those four directories. The shadow
build itself is the documented one:

```
mkdir build/CM && cd build/CM
/usr/bin/qmake6 ../../Project.pro
env MAKE=/usr/bin/make /usr/bin/make -j16 QMAKE=/usr/bin/qmake6
```

## 7. What wasi-sdk 34.0 consists of

Recorded because the guidance is that the *contents* travel with the report even
though the installation directory does not. The release is a clang distribution
plus a `wasi-sysroot`:

| Part | Contents |
|---|---|
| Compilers | `clang` 23.1.0 (default target `wasm32-unknown-wasip1`), `clang++`, `wasm32-wasip1-clang(++)`, `wasm32-wasip1-threads-clang(++)`, `wasm32-wasip2-clang(++)`, `wasm32-wasip3-clang(++)` |
| Linker | `wasm-ld`, and `wasm-component-ld` — the piece that makes `wasm32-wasip2-clang` emit a **component**, not a core module |
| Binary tools | `wasm-opt`, `wasm2wat`, `wat2wasm`, `wasm-objdump`, `wasm-validate`, `wasm-strip`, `wasm-merge`, `wasm-metadce`, `wasm-decompile`, … (the Binaryen/LLVM utility set) |
| Sysroot | `include/` (`wasm32-wasip1`, `wasm32-wasip1-threads`, `wasm32-wasip2`, `wasm32-wasip3`, plus `c++/`) |
| Sysroot libraries | `lib/wasm32-wasip1`, `lib/wasm32-wasip1-threads`, `lib/wasm32-wasip2`, `lib/wasm32-wasip3` — `libc.a`, `libc.so`, `libm`, `libc++`, `libc++abi`, `libunwind`, `libsetjmp`, `libdl`, `libpthread`, `libresolv`, `librt`, `libutil`, `crt1.o`/`crt1-reactor.o`/`crt1-command.o` |
| Extras | `experimental-coop-threads/`, `share/` |

The two facts that matter for this set: the `wasm32-wasip2` target is the one
that emits a component directly (`wasm-component-ld`), and `-mexec-model=reactor`
is what selects `crt1-reactor.o` over a command's `_start`.

## 8. What was not verified

| Not done | Consequence |
|---|---|
| Each yellow arm reduced to its exact failing `canon` definition | the finding is proven by the `u64` probe, but per-arm attribution is inferred from the import lists, not measured |
| Any WASI 0.2 host shim | no preview2 component was ever instantiated, so nothing is known about how much of `wasi:io` the component runtime can satisfy |
| Strings, lists, records, variants, resources, streams, futures, async | the worlds are scalar-only; the Canonical ABI's interesting half is untested |
| Aggregates and `post-return` | the same |
| A second path per language (.NET alternative targets, Go native `wasip2`, `wasm32-wasip1` + `--adapt`) | one documented path per language, as the guide defines it |
| Performance, size ceilings, start-up cost | out of scope ([`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §10) |
| Any product change | none: `git status` shows `docs/` only, and every artifact is under the ignored `build/` |

## 9. References

- [`../LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §8.4/§8.5 — the
  evidence a verdict must carry, and the verdict rules.
- [`LF4_INDEX.md`](LF4_INDEX.md) — the verdict table and `GAP-20`.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary”.
- [`../proposals/COMPONENT_MODEL.md`](../proposals/COMPONENT_MODEL.md) — the
  component surface the runner uses.
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §4.1 and §8.1 — the
  class of `GAP-20` and its register row.
- The upstream *Building a Simple Component* pages for
  [C#](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/csharp.html),
  [C/C++](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/c.html),
  [Go](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/go.html),
  [TinyGo](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/tinygo.html),
  [Rust](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/rust.html)
  and [MoonBit](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/moonbit.html),
  all retrieved 2026-11-16.
- [CanonicalABI.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md)
  — `flatten_functype` and `flatten_type`, retrieved 2026-11-16.

ones also pass the engine's loader.
