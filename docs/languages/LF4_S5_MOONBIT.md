# `S5` — MoonBit {#lf4_s5}

Status: `LF-4` arm report (revision 1 — 2026-11-16). Arm of
[`LF4_INDEX.md`](LF4_INDEX.md); toolchain pinned in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2. **Verdict: 🟩 Green** — the component
loads, instantiates and returns 42, and at 3,236 bytes it is the smallest
artifact in the set.

Audience: an embedder considering MoonBit, and a reader of the upstream MoonBit
page (whose generator output has moved since that page was written).

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was run, and where the current `wit-bindgen` differs from the guide |
| §3 | The artifact and its transcript |
| §4 | What the arm shows |
| §5 | What it does not answer |
| §6 | References |

## 2. What was run

```
wit-bindgen moonbit wit/world.wit --out-dir . \
    --derive-eq --derive-show --derive-error
moon build --release
wasm-tools component embed wit _build/wasm/release/build/gen/gen.wasm \
    --encoding utf16 --output adder.core.wasm
wasm-tools component new adder.core.wasm --output adder.component.wasm
```

Three drifts from the upstream page, all from `wit-bindgen` 0.63.0 and MoonBit
0.1.20261028 rather than from the engine:

| Guide says | This toolchain does |
|---|---|
| generated `moon.mod.json`, `*.pkg.json` | `moon.mod` and `moon.pkg` |
| a generated `stub.mbt` with an unfinished function | `gen/interface/docs/adder/add/top.mbt` declares `pub fn add`; a `stub.mbt` beside it with `pub fn add(x : UInt, y : UInt) -> UInt { x + y }` satisfies it (`moon check` is clean) |
| `target/wasm/release/build/gen/gen.wasm` | `_build/wasm/release/build/gen/gen.wasm` |

The generated `gen/moon.pkg` already carries the component link options
(`"wasmExportAdd:docs:adder/add@0.1.0#add"`, `"memory"`, heap start 16), so the
two `wasm-tools` commands are the whole conversion — `embed` attaches the WIT
and `new` lifts the core export.

## 3. The artifact and its transcript

| Property | Value |
|---|---|
| Size | 3,236 bytes |
| SHA-256 | `662c80a97a43e6b44bd0ce5cd3cca3e2e8f3d48ce89b37a79777344afe5e176f` |
| Imports | none |
| Export | `docs:adder/add@0.1.0` |
| `wasm-tools validate --features all` | exit 0 |

```
$ component_probe adder.component.wasm --bagcall 'docs:adder/add@0.1.0/add:20:22'
loaded path=…/adder.component.wasm backend=libjit size=3236
features=baseline,values,async,threads,component-imports,memory64,error-context
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

## 4. What the arm shows

A component does not have to be a runtime. MoonBit's 3,236-byte artifact is the
whole adder component — core module, WIT metadata and wrapper — and it runs
through the same public surface as the 2.2 MB C# one would after `GAP-20`. The
`--encoding utf16` option is MoonBit's WIT convention rather than an engine
requirement; the engine reads the encoding from the component's own canonical
options.

## 5. What it does not answer

- Strings, arrays and MoonBit's GC types — the generated bindings support them,
  the scalar-only world does not exercise them.
- The async and error-derive paths the generator offers — untested.
- Whether MoonBit's own runtime services would join the preview2-import family
  for a non-trivial program — untested.

## 6. References

- [`LF4_INDEX.md`](LF4_INDEX.md) §4–§5 — verdicts and `GAP-20`.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2–§5 — versions, hash, transcript.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary”.
- [`LF2_S4_WASMGC.md`](LF2_S4_WASMGC.md) — MoonBit as a **core-module**
  producer (`T86`), which is what `SCOPE.md` §7.1 claims.
- [The upstream MoonBit page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/moonbit.html),
  retrieved 2026-11-16.
