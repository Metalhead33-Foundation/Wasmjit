# `S5` — Guest Scripting VMs {#lf2_s5}

Status: `LF-2` cluster report (revision 1 — 2026-10-30). Cluster `L5` of
[`CLUSTERING.md`](CLUSTERING.md) §3.1; rows `T26` (Lua), `T35` (Berry), `T43`
(Janet), `T60` (MicroPython) and `T67` (mruby). **Lua was built from source and
run**; the other four were not (`LF2_L6_L12.md` §5).

| Row | Arm | Verdict |
|---|---|---|
| `T26` | Lua 5.4.8, wasi-sdk 34.0 (clang 23.1.0, libc) | **Green** |

The cluster's shared question — *does a C-API-shaped VM boundary map onto the host
ABI?* — has a direct answer: the VM is an ordinary guest library, and the
embedder sees only its exported C entry points and the WASI imports its libc
declares. Lua needed **17** WASI imports and the legacy-EH rewrite; it needed no
engine change.

Audience: anyone embedding a script VM today, and `LF-3` if language work is
sequenced by cluster.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | How the module was built, and why the flags are the interesting part |
| §3 | The transcript, including a script calling a native import |
| §4 | The criteria table |
| §5 | Memory layout and start-up |
| §6 | What the cluster leaves open |

## 2. The arm

Lua has no official WebAssembly target, which is why `LF-1` grouped it as a
*guest VM* rather than a compiler: the source is portable C, and the toolchain is
whatever C toolchain the embedder already uses. The arm compiles every Lua
source file except the two `main()` programs and links a small shim:

```
clang --target=wasm32-wasip1 -O2 -mexec-model=reactor \
      -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
      -fwasm-exceptions -mllvm -wasm-enable-sjlj \
      -include lua_config.h -Ilua-5.4.8/src <lua sources> shim.c \
      -lwasi-emulated-signal -lwasi-emulated-process-clocks -lsetjmp -lm
```

Three build facts matter:

1. **`setjmp`/`longjmp` is Lua's error mechanism**, and wasi-libc refuses it
   without `-mllvm -wasm-enable-sjlj`, which lowers it through the exception
   proposal. The resulting module uses the **legacy** `try` encoding, so a
   **strict** parse refuses it: `legacy exception handling … try (0x06)`.
2. **The default recovery policy accepts it.** No environment variable and no
   `ParseOptions` change: the load rewrites the bodies that use the form and
   records `WASM.DECODE.LEGACY_EXCEPTIONS_REWRITTEN`.
   `ParseOptions::acceptingLegacyExceptionHandling()` (or `WASMJIT_LEGACY_EH=1`)
   still forces the unconditional rewrite and stays quiet. This is the contrast
   with `F2`: clang's C++ output needed the escape-block translation for its
   `delegate` past a catch (it was refused before `LE-8` closed), while Lua's
   `setjmp`-shaped output was already inside what the rewrite handles.
3. **Two libc gaps are the embedder's**: `tmpfile` and `system` do not exist on
   WASI, and Lua references them, so the shim defines both (returning `NULL` and
   `-1`). That is the kind of thing a VM port always carries; it is shim work,
   not engine work.

The module: 602,204 bytes, 18 imports (`env.host_add` + 17 WASI), 6 exports
(`_initialize`, `lua_answer`, `lua_run`, `lua_host`, `lua_print`, `memory`), one
memory.

## 3. The transcript

```
call _initialize/void() -> void
call lua_answer/i32() -> i32=42          # plain guest code
call lua_run/i32()  -> i32=42            # luaL_newstate + luaL_dostring("return 6*7")
call lua_host/i32() -> i32=42            # a script calls env.host_add through a registered C function
call lua_print/i32()hello from Lua Lua 5.4
 -> i32=0
memory memory: 131072 bytes (2 pages) max=4294967296 shared=0
host_add_calls=1
```

`lua_host` is the bar's item 2 done properly for a VM: `lua_register` publishes a
native host function to the script, and the script's `return host_add(19, 23)`
produces 42 through the engine's import path. `lua_run` is the item the charter
raises for `L5` ([`CHARTERS.md`](CHARTERS.md) §2.1): a script executes inside the
VM before `callCallable` is declared to work.

## 4. The criteria table

| Criterion | `T26` |
|---|---|
| Export through `Callable` | yes (`lua_*` entry points; the VM's own C API) |
| Native host import | yes — `env.host_add`, called from a Lua chunk |
| Second module's export | not run; not applicable to a self-contained VM ([`CHARTERS.md`](CHARTERS.md) §2.1 item 3 allows this when stated) |
| Single-agent contract | yes; one VM state at a time, no scheduler |
| Engine patch | no; the legacy-EH rewrite is existing engine behaviour, selected per load |
| WASI imports | 17 |
| Heap owner | Lua's own allocator over wasi-libc `malloc` |
| Declared maximum | 4 GiB |
| `Reserved` usable | yes |
| `setjmp` path | `-mllvm -wasm-enable-sjlj`, legacy encoding, rewrite required |

## 5. Memory layout and start-up

One memory, index 0, two pages at `_initialize`, 4 GiB maximum, grown by Lua's
allocator. No start section: a reactor's `_initialize` runs libc's init and
returns, and every VM state is created explicitly by the host or by the shim.
That is the embedding shape a game wants: instantiate once, `lua_newstate` per
script environment, close the state when done.

## 6. What the cluster leaves open

- The other four members. `T60` (MicroPython) and `T67` (mruby) are also
  C-source VMs and would be the cheapest next arms; `T43` (Janet) and `T35`
  (Berry) are smaller. None was built here, and each is a `C3`-band arm only
  because obtaining and patching the source is work, not because the engine
  question is open.
- A VM that requires a *scheduler* (MicroPython's `uasyncio` is cooperative,
  mruby's fibres are guest-side). Neither was tested; the `Single` contract only
  bites a VM that wants a second host thread.
- Whether the legacy-EH rewrite accepts every VM's `setjmp` output. Lua's does;
  that is one data point, not a guarantee.
