# `S3` — Go and TinyGo {#lf4_s3}

Status: `LF-4` cluster report (revision 3 — 2026-11-16). Two arms of
[`LF4_INDEX.md`](LF4_INDEX.md); toolchains pinned in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2.

| Arm | Toolchain | Verdict |
|---|---|---|
| `s3-go` | Go 1.27.4 + `componentize-go` 0.4.2 | 🟩 **Green** — after `GAP-25`; the red that looked like a toolchain defect reproduced on no other host |
| `s3-tinygo` | TinyGo 0.40.1, `-target=wasip2` | 🟩 **Green** — after `GAP-20` and `GAP-24` |

Both toolchains build valid components, and both components used to be rejected
at **load** by the same validator rule; both now load and reach the host-import
seam. The two paths differ in almost everything
else — a 2.5 MB runtime-heavy component versus a 22 KB one — which is useful:
the blocker is not the runtime's size or complexity, it is the WASI import list
that both carry.

Audience: an embedder considering Go, and whoever sequences `LF-3`.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | The Go arm |
| §3 | The TinyGo arm, including the version constraints that bit |
| §4 | What the engine did |
| §5 | What it would take to run either |
| §6 | What this cluster does not answer |
| §7 | References |

## 2. The Go arm

The guide's steps, in order:

```
mkdir adder && cd adder
# wit/adder.wit: the adder world, unchanged
componentize-go --world adder bindings
go mod tidy
# export_docs_adder_add/exports.go:
#   package export_docs_adder_add
#   func Add(x uint32, y uint32) uint32 { return x + y }
componentize-go build
```

`componentize-go` writes `wit_exports.go` (the `//go:wasmexport` shims, generated
by wit-bindgen 0.61.1), a `go.mod` with module name `wit_component`, and then
builds `main.wasm`. The `--world adder bindings` step needs the WIT in `./wit`.

| Property | Value |
|---|---|
| Size | 2,524,500 bytes |
| SHA-256 | `d53945a962544106113c411e27b527c1d04252fa4912a5131dac406912621d4e` |
| Imports | 18 WASI 0.2.12 interfaces: `wasi:cli/{environment,exit,stdin,stdout,stderr,terminal-*}`, `wasi:io/{error,poll,streams}`, `wasi:clocks/{monotonic,wall}`, `wasi:filesystem/{types,preopens}`, `wasi:random/random` |
| Export | `docs:adder/add@0.1.0` |
| `wasm-tools validate --features all` | exit 0 |

The artifact is a whole Go runtime in a component: 2.5 MB, and the adder function
is a few bytes of it.

## 3. The TinyGo arm

TinyGo's path is the more involved one, and three version constraints are worth
recording because they are not in the guide:

1. **The world must include the CLI imports.** TinyGo's `wasip2` target assumes a
   `wasi:cli/command` shape, so the guide's world is
   `world adder { include wasi:cli/imports@0.2.0; export add; }`.
2. **The imports must resolve.** The `wasi:cli@0.2.0` package and its
   `wasi:{io,clocks,filesystem,sockets,random}` dependencies were taken from the
   TinyGo installation's own `lib/wasi-cli/wit` and placed under `wit/deps/`,
   then encoded to a binary WIT package with
   `wasm-tools component wit wit --wasm -o docs:adder@0.1.0.wasm`.
3. **TinyGo 0.40.1 refuses a Go 1.27 front end.** The bindings were generated
   with the newer Go (`go get -tool go.bytecodealliance.org/cmd/wit-bindgen-go`,
   `go tool wit-bindgen-go generate --world adder --out internal`), and the build
   then needed `GOTOOLCHAIN=go1.25.6` plus lowering the generated `go 1.27.4`
   directive to `1.25.6`.

The build itself is the guide's:

```
tinygo build -target=wasip2 -no-debug -o adder.wasm \
    --wit-package docs:adder@0.1.0.wasm --wit-world adder main.go
```

| Property | Value |
|---|---|
| Size | 22,207 bytes |
| SHA-256 | `db22255abdcb6c9ea502e8a9ffc191102bfd49b8ff39bb1132b6a60e6520cb2c` |
| Imports | 4 WASI 0.2.0 interfaces: `wasi:io/error`, `wasi:io/streams`, `wasi:cli/stdout`, `wasi:random/random` |
| Export | `docs:adder/add@0.1.0` |
| `wasm-tools validate --features all` | exit 0 |

The upstream TinyGo page carries a warning that its component-model tooling is
not currently maintained and recommends Go; that warning is consistent with what
this arm needed — the guide's `wkg`-based workflow, the newer `wit-bindgen-go`,

## 4. What the engine did

**First run (`6164c53`).** Both arms failed identically and immediately:

```
status=error: component validation: canonical option `memory` is required
```

`wasm-tools validate --features all` accepted both bytes. That was `GAP-20`
([`LF4_INDEX.md`](LF4_INDEX.md) §5.1), in the loader, before any import was
resolved.

**Second run (`1ec91f8`).** Both artifacts now load and validate, and the two
arms stop at *different* places, which is the interesting part:

```
>>> s3-go
status=error: component runtime: missing host import `wasi:cli/environment@0.2.12`
>>> s3-tinygo
status=error: component runtime: missing host import `wasi:io/error@0.2.0`
```

Go needed a second validator fix beyond `GAP-20`: `componentize-go` wraps a
`wasip1` core module with the preview1 adapter, whose memory import is
`(memory 0)` while the instance it is handed is 36 pages, and the engine compared
the limits for equality — `GAP-22`
([`LF4_INDEX.md`](LF4_INDEX.md) §5.3). TinyGo's 22 KB component has no adapter and
went straight from load failure to the host seam.

The difference between the two artifacts is still instructive: 2.5 MB and 18
imports versus 22 KB and 4 imports makes no difference to the *kind* of blocker.
What the engine reacts to is the shape of the artifact — an adapted preview1
module or not — not the size of the runtime.

## 5. What it would take to run either

1. **A WASI 0.2 shim** — class D, the embedder's ([`../SCOPE.md`](../SCOPE.md)
   §3). TinyGo's four imports are a much smaller shim than Go's eighteen: both
   need `wasi:io/streams` (resources and a write path) and `wasi:random`, and
   TinyGo's `wasi:cli/stdout` is the narrowest useful output surface in this set.
   If the goal is “run a Go-family component soon”, TinyGo is the cheaper target
   even though upstream calls its tooling unmaintained.

## 6. What this cluster does not answer

- **Go's native `wasip2` build** (without `componentize-go`) — not run; the guide
  documents `componentize-go`, and one path per language is this set's rule.
- **Any aggregate or string** — the world is scalar-only.
- **Whether either runtime's start-up would work under the engine once the WASI
  shim exists** — untested; the loader never gets that far.
- **Whether the Go runtime's GC, scheduler or `//go:wasmexport` shims interact
  with the engine's single-agent contract** — untested.

## 7. References

- [`LF4_INDEX.md`](LF4_INDEX.md) §4–§5 — verdicts and the three findings.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2–§5 — versions, hashes, transcripts,
  reproduction.
- [`../ABI.md`](../ABI.md) “Host and Guest: the vocabulary”.
- [`../SCOPE.md`](../SCOPE.md) §3 — WASI as the embedder's surface.
- [`LF2_S2_LLVM.md`](LF2_S2_LLVM.md) — the same two toolchains as **core-module**
  producers, which is what `SCOPE.md` §7.1 claims.
- [The upstream Go page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/go.html)
  and [TinyGo page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/tinygo.html),
  retrieved 2026-11-16.

and a Go front-end downgrade.
