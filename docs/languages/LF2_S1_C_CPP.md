# `S1` — C/C++ Toolchain and ABI {#lf2_s1}

Status: `LF-2` cluster report (revision 1 — 2026-10-30). Cluster `L1` of
[`CLUSTERING.md`](CLUSTERING.md) §3.1; rows `T01`–`T05` of
[`TRIAGE.md`](TRIAGE.md) §5.1, with `T36`/`T94` as fixtures. Five arms were
built and run through the documented recipe. Verdicts, with the engine at
`9a7b539` and the toolchains pinned in [`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §2:

| Row | Arm | Verdict |
|---|---|---|
| `T02` | C, clang 23.1.4, `--target=wasm32`, `-nostdlib` | **Green** |
| `T03` | C, wasi-sdk 34.0, `-mexec-model=reactor` | **Green** |
| `T01` | C, Emscripten 6.0.13, `-sSTANDALONE_WASM=1`, `-O2` | **Green** (after `F1`; it was yellow at `-O1`) |
| `T04` | C++, Emscripten 6.0.13, reactor, `-O2` | **Green** (after `F1`) |
| `T05` | C++, wasi-sdk 34.0 with libc++ | **Yellow** (`F2`: default legacy EH trips `LE-8`; a flag or one `wasm-opt` pass clears it) |

Audience: an embedder choosing a C/C++ toolchain today, and whoever sequences
`LF-3`.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was in scope, and what was deliberately not? |
| §3 | The five arms: build command, artifact, transcript |
| §4 | The §4 criteria table, filled per arm |
| §5 | Memory layout and start-up, stated as measured |
| §6 | Cross-module linking |
| §7 | Engine work this cluster needs, named and classed |
| §8 | What the cluster does not answer |

## 2. Scope

In: exports and `Callable` mapping; a native host import; a second module's
export; the single-agent contract; memory layout and start-up; the WASI surface
each toolchain actually declares. Out: threads, SIMD performance, DWARF, C++
standard-library completeness, and the internal cause of `F1` (it is reported,
not diagnosed).

The four toolchain paths are the ones `LF-1` clustered: clang with no libc,
wasi-sdk's libc, wasi-sdk's libc++, and Emscripten's standalone runtime. They
answer one question each; they do not compete.

## 3. The arms

### 3.1 `T02` — C, clang, no libc (Green)

```
clang --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--allow-undefined answer.c
```

`t02_answer.wasm`: 468 bytes, 1 import (`env.host_add`), 7 exports, one memory
of 2 pages. Transcript:

```
call answer/i32() -> i32=42
call add/i32(19,23) -> i32=42
call via_host/i32(19,23) -> i32=42      # host_add_calls=1
call heap_ptr/i32() -> i32=65536        # static data starts after the first page
memory memory: 131072 bytes (2 pages) max=4294967296 shared=0
```

Everything the bar asks for is in one 468-byte module: an export reached through
`Callable`, a native import, a guest-owned heap, and a declared maximum. There is
no runtime to initialise and no start function.

### 3.2 `T03` — C, wasi-sdk, libc (Green)

```
clang --target=wasm32-wasip1 -O2 -mexec-model=reactor wasi_hello.c
```

`t03_reactor.wasm`: 60,189 bytes, 4 imports (`env.host_add` plus `fd_write`,
`fd_seek`, `fd_fdstat_get`), 7 exports, `_initialize` exported as a reactor
entry. Transcript:

```
call _initialize/void() -> void
call answer/i32() -> i32=42
call via_host/i32(19,23) -> i32=42
call sum_alloc/i32(100) -> i32=5050     # malloc/free over the guest heap
call print_hello/void()hello from wasi-sdk on wasm32-wasi
```

The WASI surface is **three functions**, and one of them (`fd_fdstat_get`) exists
only because `printf` probes the stream. A reactor is the right shape for an
embedder: `_initialize` runs libc's constructors, then exports are ordinary
calls. `max=4294967296` is the memory's declared maximum, so
`MemoryAllocationPolicy::Reserved` is usable (`MP-1`).

### 3.3 `T01` — C, Emscripten standalone (Yellow)

```
emcc em_main.c -O2 -sSTANDALONE_WASM=1 -sEXPORTED_FUNCTIONS=_answer,_add,_via_host
```

`t01_emcc.wasm`: 1,742 bytes, 3 imports (`env.host_add`, `proc_exit`,
`fd_write`), 9 exports, a **fixed** memory of 258 pages (16.5 MiB,
`initial-heap=16777216`, `-z stack-size=65536`). The module imports far less
than the triage predicted, and there is no JS glue in it.

It does not instantiate: `jit_function_compile failed 0 for func 2`, then
`std::abort()` from `LibjitModuleCompiler.cpp:234`, while compiling `_start`
(the fifth defined body). `-O0` and `-O1` of the same source are green:

```
$ probe t01_emcc_o1.wasm --wasi --env --call answer/i32 --call _start/void
call answer/i32() -> i32=42
call _start/void()hello from Emscripten standalone
wasi_exit=0
```

So the module shape was fine and the failure was a backend lowering problem in
one optimised function. `F1` was studied in depth and fixed: libjit's level-1
optimizer rejects the block graph, level 0 compiles it, and the backend now
defaults to level 0 with `WASMJIT_LIBJIT_OPT` as the opt-in
([`LF2_DEEP_F1_BACKEND.md`](LF2_DEEP_F1_BACKEND.md)). At this engine commit the
`-O2` module loads, runs and exits 0; the transcript above is the `-O1` one that
proved the module shape before the fix.

### 3.4 `T04` — C++, Emscripten standalone (Green after `F1`)

```
em++ em_main.cpp -O1 -sSTANDALONE_WASM=1 --no-entry \
     -sEXPORTED_FUNCTIONS=_ctor_value,_sum_vector,_string_len
```

`t04_empp_reactor.wasm`: 11,613 bytes, 2 imports (`proc_exit`, `fd_write`), one
fixed memory of 258 pages. Transcript:

```
call _initialize/void() -> void
call ctor_value/i32() -> i32=40        # static constructor ran
call sum_vector/i32(6) -> i32=21       # std::vector
memory memory: 16908288 bytes (258 pages) max=16908288 shared=0
```

`--no-entry` produces the reactor shape (`_initialize`, no `_start`), which is
what an embedder wants: constructors run once, then the exports are callable.
At `-O2` the same `F1` failure occurred before the fix; it is now fixed in the
backend. **Green at this engine commit.**

### 3.5 `T05` — C++, wasi-sdk with libc++ (Yellow)

Two builds of the same source. The default one — `clang++ -fwasm-exceptions`,
which is what a user gets without knowing about LLVM's wasm flag — contains a
legacy `try` (0x06) that the rewrite cannot translate:

```
status=error: legacy exception handling: this module uses a construct the
built-in rewrite does not translate (delegate that skips an intervening catch)
```

That is `LE-8`, the open row of
[`proposals/LEGACY_EXCEPTIONS.md`](../proposals/LEGACY_EXCEPTIONS.md) §4.6, and
it is what `F2` records. With the documented flag the module is green:

```
clang++ --target=wasm32-wasip1 -O2 -fwasm-exceptions \
        -mllvm -wasm-use-legacy-eh=false -mexec-model=reactor \
        hello.cpp -lc++ -lc++abi -lunwind -lsetjmp
```

```
call _initialize/void() -> void
call ctor_value/i32() -> i32=40
call via_host/i32(19,23) -> i32=42
call sum_vector/i32(6) -> i32=21
call throw_catch/i32(21) -> i32=42     # try/catch inside the guest
call throw_catch/i32(-1) -> i32=-1     # exception caught in the guest
```

The C++ runtime (libc++, libc++abi, libunwind, the `setjmp` emulation) costs
336 KB; nothing in it needs a host service beyond the six WASI imports listed in
[`LF2_EVIDENCE.md`](LF2_EVIDENCE.md) §3.

## 4. The criteria table

| Criterion | `T02` clang C | `T03` wasi-sdk C | `T01` emcc C | `T04` emcc C++ | `T05` wasi C++ |
|---|---|---|---|---|---|
| Exports reachable through `Callable` | yes | yes | yes (`-O1`) | yes (`-O1`) | yes |
| Native host import | `env.host_add` | `env.host_add` | `env.host_add` | — (not declared) | `env.host_add` |
| Second module's export | yes (`dep.add`) | not run | not run | not run | not run |
| Single-agent contract | yes | yes | yes (`-O1`) | yes (`-O1`) | yes |
| Engine patch required | no | no | no (fixed in the backend) | no (fixed in the backend) | **yes** for default output (`F2`); no with the flag or one `wasm-opt` pass |
| Start-up that must run first | none | `_initialize` | `_start` (runs `main`) | `_initialize` | `_initialize` |
| WASI imports | 0 | 3 | 2 | 2 | 6 |
| Guest heap owner | guest (`static char[]`) | wasi-libc `malloc` | Emscripten `dlmalloc` | libc++ over dlmalloc | wasi-libc `malloc` |
| Declared memory max | none (grows) | 4 GiB | fixed 258 pages | fixed 258 pages | 4 GiB |
| `Reserved` policy usable | no (no max) | yes | n/a (fixed) | n/a (fixed) | yes |

The `—` in `T04` is honest: the Emscripten C++ arm was built without a host
import, because the `env` attribute is a clang feature the em++ driver honours
(`T01` proves it) and repeating it in C++ would not test anything new. That is a
scope decision, not a pass.

## 5. Memory layout and start-up

| Arm | Memory index | Initial | Max | Base stability | Who grows it |
|---|---|---|---|---|---|
| `T02` | 0 | 2 pages | unbounded | moves on `memory.grow` (Growable) | the guest, explicitly |
| `T03` | 0 | 2 pages | 4 GiB | moves unless `Reserved` | `malloc` |
| `T01`/`T04` | 0 | 258 pages | 258 pages | **never moves** (non-growable) | nobody; `malloc` fails at the cap |
| `T05` | 0 | 2 pages | 4 GiB | moves unless `Reserved` | `malloc` |

The embedder decides one thing that matters: `MemoryAllocationPolicy::Reserved`
on a module with a declared maximum keeps `memoryBase` stable, which is what a
runtime that caches raw pointers into linear memory needs
([`proposals/MEMORY_PROVIDER.md`](../proposals/MEMORY_PROVIDER.md) §4.7). No arm
needed a second memory; multi-memory is available but unused by these
toolchains, which is the answer to that question for `S1`.

Start-up is the sharpest difference: an Emscripten `_start` command module runs
`main` and exits through `proc_exit`; wasi-sdk reactors and `--no-entry`
Emscripten modules expose `_initialize` and never exit. An embedder that wants
exports to survive must pick the reactor shape.

## 6. Cross-module linking

`T02` was the cross-module arm: `t02_dep.wasm` exports `add`, `t02_caller.wasm`
imports `dep.add`, and the runner instantiates the dependency first and
publishes its `Callable` under the importer's module name
([`ABI.md`](../ABI.md) "Cross-module Wasm function imports"):

```
linked module=dep path=t02_dep.wasm
call call_dep/i32(19,23) -> i32=43      # dep.add(19,23) + 1
```

No engine work, no JS glue, one instance per module — the bar's item 3 passes
for the toolchain family. The same shape is available to the other arms; it was
not repeated because the mechanism is the engine's and already proven.

## 7. Engine work

| Finding | Class | What it blocked | Status |
|---|---|---|---|
| `F1` — LibJIT aborts compiling an `-O2` Emscripten `_start` | **B** (per-backend) | `T01`, `T04` at `-O2` | **fixed**: level-0 default, `WASMJIT_LIBJIT_OPT` opt-in, `WASM.COMPILE.FUNCTION_FAILED` instead of `abort()` ([`LF2_DEEP_F1_BACKEND.md`](LF2_DEEP_F1_BACKEND.md)) |
| `F2` — the legacy-EH rewrite refuses `LE-8` | **A** (core parser) | `T05` at default flags, and every toolchain that cannot re-flag | open; the flag and `wasm-opt --translate-to-exnref` are the documented workarounds ([`LF2_DEEP_F2_EH.md`](LF2_DEEP_F2_EH.md)) |

Both are `LF-3` items. Neither changes the ABI, the embedding surface or the
backend contract, which is why `S1` does not disturb the frozen surfaces of
[`API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §2.2.

## 8. What this cluster does not answer

- The internal cause of `F1`, and whether every `-O2` Emscripten module fails or
  only ones shaped like `_start`. A reduction is `LF-3` work.
- Whether the same module behaves differently on `Sljit`/`Asmjit`; only `libjit`
  was selected, and `F1` may not be libjit-specific.
- C++20 modules, coroutines, `std::thread` and `<filesystem>`: none was in
  scope, and none is needed to pass the bar.
- `T36` (c4wa) and `T94` (xcc). They are fixture generators: a C-subset emitter
  proves nothing an embedder would rely on, and `LF-1` already assigned them to
  `S1` as fixtures rather than candidates.
