# `S2` — LLVM-Family Languages {#lf2_s2}

Status: `LF-2` cluster report (revision 1 — 2026-10-30). Cluster `L2` of
[`CLUSTERING.md`](CLUSTERING.md) §3.1; rows `T06`, `T07`, `T10`, `T33` were run;
`T34` (Ada), `T38` (Crystal), `T39` (D), `T56` (Pascal), `T72` (Swift) and `T88`
(Nim) share the cluster and were not run ([`LF2_L6_L12.md`](LF2_L6_L12.md) §5).

| Row | Arm | Verdict |
|---|---|---|
| `T06` | Rust 1.99.1, `wasm32-unknown-unknown` | **Green** |
| `T07` | Rust 1.99.1, `wasm32-wasip1` | **Green** |
| `T10` | TinyGo 0.40.1, `-target=wasi` | **Green** |
| `T33` | Zig 0.15.2, `wasm32-freestanding` | **Green** |

The cluster's prediction held: once the toolchain emits a standard module, the
only question is which host imports it declares. None of the four needed an
engine change, and the largest shim was four WASI functions (`T07`). Go (`T09`)
belongs to `L8`, not this cluster, and is reported with the coverage sweep.

Audience: an embedder choosing a Rust/Go/Zig-class toolchain; `LF-3` when it
sequences language work.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What each arm was built with, and what the module contains |
| §3 | The transcripts, including the negative controls |
| §4 | The criteria table |
| §5 | Memory layout and start-up |
| §6 | What the cluster leaves open |

## 2. The arms

| Row | Build | Bytes | Imports | Exports |
|---|---|---:|---:|---:|
| `T06` | `rustc --target wasm32-unknown-unknown -O --crate-type=cdylib` | 1,653,586 | 1 (`env.host_add`) | 6 |
| `T07` | `rustc --target wasm32-wasip1 -O --crate-type=cdylib` | 2,167,021 | 5 (1 `env` + 4 WASI) | 4 |
| `T10` | `tinygo build -target=wasi -opt=0` | 1,571,287 | 7 (WASI) | 13 |
| `T33` | `zig build-exe -target wasm32-freestanding -O ReleaseSmall -fno-entry -rdynamic` | 108 | 0 | 4 |

Two build details are load-bearing. `T33` needs `-rdynamic`; without it Zig
strips every `export fn` and emits a 36-byte empty module, which would look like
a toolchain defect and is not. `T10` needs `-opt=0` in this environment because
TinyGo's bundled `wasm-opt` step fails on the container's `/tmp` view; the
unoptimised module is 1.5 MB and behaves identically. Both are flags, not
engine questions.

## 3. Transcripts

`T06` — no WASI, one host import, `Vec`/`String` over `dlmalloc`:

```
call answer/i32() -> i32=42
call add/i32(19,23) -> i32=42
call via_host/i32(19,23) -> i32=42
call sum_vec/i32(100) -> i32=5050
memory memory: 1179648 bytes (18 pages) max=4294967296 shared=0
```

`T07` — the same source shape with `std` and a WASI surface:

```
call answer/i32() -> i32=42
call via_host/i32(19,23) -> i32=42
call greet/void()hello from rustc on wasm32-wasip1
```

`T10` — exports work **before** `_start`, and `_start` runs `main`:

```
call answer/i32() -> i32=42
call add/i32(19,23) -> i32=42
call _start/void()hello from TinyGo
wasi_exit=0
```

`T33` — no imports at all, and a memory the guest never grows:

```
call answer/i32() -> i32=42
call add/i32(19,23) -> i32=42
memory memory: 1048576 bytes (16 pages) max=4294967296 shared=0
```

Every arm passes the bar's item 1 and item 4 outright. Item 2 (a native host
import) is exercised in `T06`/`T07` through `env.host_add`; `T10` and `T33`
declare no import, because the registration path is the same one `T01`/`T03`
use and repeating it would not test anything new. Item 3 (cross-module) is
engine-side and was proven once on `T02`.

## 4. The criteria table

| Criterion | `T06` Rust | `T07` Rust/WASI | `T10` TinyGo | `T33` Zig |
|---|---|---|---|---|
| Export through `Callable` | yes | yes | yes | yes |
| Native host import | yes | yes | not declared | not declared |
| Single-agent contract | yes | yes | yes | yes |
| Engine patch | no | no | no | no |
| WASI imports | 0 | 4 | 7 | 0 |
| Heap owner | Rust `dlmalloc` | Rust `dlmalloc` | TinyGo allocator | none (no allocation) |
| Initial memory | 18 pages | 17 pages | 2 pages | 16 pages |
| `Reserved` usable | yes (max 4 GiB) | yes | yes | yes |
| Notable caveat | `panic=abort`; unwinding needs a flag | same | needs Go ≤ 1.25 for the driver, and `-opt=0` here | needs `-rdynamic` to keep exports |

## 5. Memory layout and start-up

| Arm | Memory index | Runtime init | Who grows |
|---|---|---|---|
| `T06` | 0 | none (no start function) | the allocator |
| `T07` | 0 | `main` is not run unless `_start` is called | the allocator |
| `T10` | 0 | `_start` runs `runtime_init` + `main` | the runtime; 2 pages at `_initialize` |
| `T33` | 0 | none | nobody; a freestanding module that does not allocate |

`T06` is the cleanest embedding shape in the whole study: a module with no start
function, one import, and 18 pages that grow on demand. `T10` is the most
forgiving of the "runtime" arms because its exports are callable before
`_start`; `T09` is not (see [`LF2_L6_L12.md`](LF2_L6_L12.md) §3).

## 6. What the cluster leaves open

- The six untested members. Each needs its own toolchain install (D's LDC, Swift's
  SwiftWasm SDK, Ada's GNAT-LLVM, Pascal's FPC, Nim's `nlvm`, Crystal's
  unreleased PR) and none was present; [`LF2_L6_L12.md`](LF2_L6_L12.md) §5 keeps
  their `LF-1` dispositions and names the trigger.
- Rust with unwinding: `panic=abort` is the tested configuration. A
  `panic=unwind` build is expected to hit the same legacy-EH question as `T05`
  and was not built.
- Whether `-O2` Emscripten's `F1` has an analogue in any of these backends. None
  of these arm's default flags triggered a backend failure, which is a useful
  negative: `F1` is not "the LibJIT backend cannot compile optimised code", it is
  one construct in one function.
