# `S4` — Rust through `wasm32-wasip2` {#lf4_s4}

Status: `LF-4` arm report (revision 1 — 2026-11-16). Arm of
[`LF4_INDEX.md`](LF4_INDEX.md); toolchain pinned in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2. **Verdict: 🟩 Green** — the component
loads, instantiates and returns 42.

Audience: an embedder choosing a component toolchain today, and a reader of the
upstream Rust page.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was run? |
| §3 | The artifact and its transcript |
| §4 | What the arm shows |
| §5 | What it does not answer |
| §6 | References |

## 2. What was run

The guide's steps, with the `wasm32-wasip2` target added to the toolchain first:

```
cargo new --lib adder
# Cargo.toml: [lib] crate-type = ["cdylib"]; wit-bindgen = "0.63.0"
# wit/world.wit: the adder world, unchanged
cargo build --target wasm32-wasip2 --release
```

`wit-bindgen`'s macro generates the bindings, and the implementation is the
generated `Guest` trait:

```rust
#[allow(dead_code)]
mod bindings {
    wit_bindgen::generate!({ path: "wit/world.wit" });
    use super::AdderComponent;
    export!(AdderComponent);
}

struct AdderComponent;

impl bindings::exports::docs::adder::add::Guest for AdderComponent {
    fn add(x: u32, y: u32) -> u32 {
        x + y
    }
}
```

The `use super::AdderComponent;` inside the module is required: the upstream
snippet's `export!(AdderComponent)` cannot see the outer struct from inside
`bindings`, and omitting the import is a plain `E0425` at build time.

## 3. The artifact and its transcript

| Property | Value |
|---|---|
| Size | 13,719 bytes |
| SHA-256 | `94b05bf7e2c721a3d8456565419efdcd53a80c77cd99a43db6c1ac8b5bcd8e96` |
| Imports | none |
| Export | `docs:adder/add@0.1.0` |
| `wasm-tools validate --features all` | exit 0 |

```
$ wasm-tools component wit adder.wasm
package root:component;
world root { export docs:adder/add@0.1.0; }
package docs:adder@0.1.0 { interface add { add: func(x: u32, y: u32) -> u32; } }

$ component_probe adder.wasm --bagcall 'docs:adder/add@0.1.0/add:20:22'
loaded path=…/adder.wasm backend=libjit size=13719
features=baseline,values,async,threads,component-imports,memory64,error-context
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

## 4. What the arm shows

`wasm32-wasip2` is a **component** target: `rustc`/`cargo` produce the component
wrapper themselves, so there is no `component new` step and no adapter. A
component that uses nothing from WASI imports nothing, and a release `cdylib` of
this size is the smallest compiled-language component in the set after MoonBit.

This is also the only arm where the upstream guide's *deprecation* note matters:
`cargo-component` is being retired in favour of the native target, and this arm
uses the native path only.

## 5. What it does not answer

- Strings, lists, records, options/results and resources — the scalar-only world
  does not exercise the Canonical ABI's lifting rules.
- `wasm32-wasip1` + `wit-bindgen` + `component new` — the older manual path, not
  run.
- `std`'s own WASI use (a Rust component that prints) — that joins the
  preview2-import family and would be yellow until `GAP-20` is fixed.

## 6. References

- [`LF4_INDEX.md`](LF4_INDEX.md) §4–§5 — verdicts and `GAP-20`.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2–§5 — versions, hash, transcript.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary”.
- [`LF2_S2_LLVM.md`](LF2_S2_LLVM.md) — Rust as a **core-module** producer.
- [The upstream Rust page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/rust.html),
  retrieved 2026-11-16.
