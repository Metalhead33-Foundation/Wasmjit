# `S2` — C and C++ through wasi-sdk {#lf4_s2}

Status: `LF-4` cluster report (revision 1 — 2026-11-16). Arms of
[`LF4_INDEX.md`](LF4_INDEX.md); toolchain pinned in
[`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2. Three arms:

| Arm | Source | Verdict |
|---|---|---|
| `s2-c` | C, `wit-bindgen c`, no libc call | 🟩 **Green** |
| `s2-cpp` | C++, `wit-bindgen cpp`, no libc call | 🟩 **Green** |
| `s2-c-libc` | the same C source plus one `printf` | 🟨 **Yellow** — `GAP-20` |

The two green arms are the study's clearest result: a component with no WIT
imports is a component the engine can load, instantiate and call today. The
yellow arm is the control that shows *why* the others are yellow: it is the same
source, the same compiler and the same flags, with one libc call.

Audience: an embedder choosing a C/C++ component toolchain, and whoever reads
the C page of the upstream guide.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What was run? |
| §3 | The artifacts and their transcripts |
| §4 | `s2-c-libc`: what one libc call changes |
| §5 | The component's internal shape |
| §6 | What this cluster does not answer |
| §7 | References |

## 2. What was run

The guide's C steps, for both languages:

```
wit-bindgen c   wit/component.wit --out-dir gen      # C
wit-bindgen cpp wit/component.wit --out-dir gen      # C++
```

The C implementation is the generated symbol:

```c
#include "gen/adder.h"

uint32_t exports_docs_adder_add_add(uint32_t x, uint32_t y)
{
    return x + y;
}
```

The C++ implementation is the generated namespace:

```cpp
#include "gen/adder_cpp.h"

namespace exports::docs::adder::add {

uint32_t Add(uint32_t x, uint32_t y)
{
    return x + y;
}

} // namespace exports::docs::adder::add
```

Both are linked with wasi-sdk's **component** driver, which is what makes the
output a component rather than a core module:

```
wasm32-wasip2-clang   -mexec-model=reactor -O2 component.c   gen/adder.c   gen/adder_component_type.o -o adder.wasm
wasm32-wasip2-clang++ -mexec-model=reactor -O2 component.cpp gen/adder.cpp gen/adder_component_type.o -o adder.wasm
```

`-mexec-model=reactor` selects the reactor C runtime (no `_start`, an
`_initialize` discipline) and `wasm-component-ld` writes the component wrapper.
`wit-bindgen`'s `*_component_type.o` carries the encoded WIT, which is why the
artifact is self-describing without a sidecar file.

## 3. The artifacts and their transcripts

| Arm | Bytes | SHA-256 | Imports | Export |
|---|---:|---|---:|---|
| `s2-c` | 50,626 | `96d7513337d3c97bb2689d763e38cb9ea7a199690788f7c17c81bf5cb7a0dfd1` | 0 | `docs:adder/add@0.1.0` |
| `s2-cpp` | 50,670 | `0d3e5bd137189bd1389c0b0f0c7dcd765b81b5d11f307b4cd369259203673ab7` | 0 | `docs:adder/add@0.1.0` |

```
$ component_probe s2-c/adder.wasm --bagcall 'docs:adder/add@0.1.0/add:20:22'
loaded path=…/s2-c/adder.wasm backend=libjit size=50626
features=baseline,values,async,threads,component-imports,memory64,error-context
instantiate=ok
call add(20,22) -> u32=42
status=ok
```

The C++ arm differs only in the last two numbers (50,670 bytes and the same
`u32=42`). Both components are **import-free** in WIT terms: the 50 KB is the
`wit-bindgen` canonical-ABI glue plus wasi-libc's `cabi_realloc`, not a WASI
surface.

## 4. `s2-c-libc`: what one libc call changes

The C source plus `printf("add(%u, %u)\n", x, y);` — nothing else changed —
produces a 274,861-byte component that imports 13 WASI 0.2.12 interfaces
(`wasi:io/{error,poll,streams}`, `wasi:cli/{stdin,stdout,stderr,terminal-*}`,
`wasi:clocks/{monotonic,wall}`). The engine's transcript is one line:

```
status=error: component validation: canonical option `memory` is required
```

`wasm-tools validate --features all` accepts it. The rejection is `GAP-20`
([`LF4_INDEX.md`](LF4_INDEX.md) §5): `wasi:clocks/monotonic-clock.now` returns a
`u64`, and the validator's flat-slot arithmetic over-counts it.

The lesson is not “printf is expensive”; it is that **the presence of a WASI
import list is what separates a green C component from a yellow one**, and that

## 5. The component's internal shape

`wasm-tools component wit` for both green arms is exactly the `adder` world with
no imports, and the component contains one core module, one memory and one bare
`canon lift`:

```
(func $add (;0;) (type 0) (canon lift (core func $docs:adder/add@0.1.0#add)))
```

That is the smallest useful component shape: a core module whose export is lifted
through the Canonical ABI with no options, because `(u32, u32) -> u32` flattens
to `(i32, i32) -> i32`. The engine resolves the export as an **instance bag**
(`exportedInstance("docs:adder/add@0.1.0")`) containing one function, which is
why the runner's `--bagcall` names the interface, not the function alone.

## 6. What this cluster does not answer

- **Strings, lists, records and `realloc`** — the whole point of the Canonical
  ABI — are untested; the world is scalar-only.
- **`wasm32-wasip1` + `wasm-tools component new --adapt`**, the guide's older
  manual path, was not run; the `wasm32-wasip2` driver is the documented one for
  this SDK.
- **C++ with libc++** (`-lc++ -lc++abi`) was not run as a separate arm. The
  C++ arm above is header-only C++ over the C ABI; a `std::string` version would
  import WASI and join `s2-c-libc` on the yellow side until `GAP-20` is fixed.
- **Performance and size** — out of scope; the byte counts identify artifacts.

## 7. References

- [`LF4_INDEX.md`](LF4_INDEX.md) §4–§5 — verdicts and `GAP-20`.
- [`LF4_EVIDENCE.md`](LF4_EVIDENCE.md) §2–§5 — toolchain pin, hashes,
  transcripts, reproduction.
- [`../ABI.md`](../ABI.md) — the core `Callable` ABI one layer below the
  Canonical ABI, and “Host and Guest: the vocabulary”.
- [`LF2_S1_C_CPP.md`](LF2_S1_C_CPP.md) — the same toolchains' **core modules**,
  which is what `SCOPE.md` §7.1 claims.
- [The upstream C/C++ page](https://component-model.bytecodealliance.org/language-support/building-a-simple-component/c.html),
  retrieved 2026-11-16.

the separator is an engine rule that can be fixed, not a property of C.
