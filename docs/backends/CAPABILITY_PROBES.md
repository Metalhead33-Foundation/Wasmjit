# Capability probes: what the tree actually does, per backend

**Status:** living note, 2026-11-06 (`backendsMh33`). Companion to
[`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md), which holds the **matrix**; this note holds
the **instruments** that fill it.

A probe is a fixture plus one test that runs it on every backend and records one of
exactly three outcomes. The rule that governs all of them:

> **A backend either does the thing, or refuses the module with a reason. A silent wrong
> answer is a defect.**

## 1. The probes

| # | Probe | Fixture | Assertion | Outcome |
|---|---|---|---|---|
| 1 | exceptions | `exceptions_probe.wat` -- a tag plus `throw` | the two refusals are `REQUIRE_THROWS`; LibJIT's wrong answer is **pinned** | SLJIT ⛔ · AsmJIT ⛔ · **LibJIT ❌** |
| 2 | `externref` | `externref_probe.wat` -- identity | if it instantiates, the reference must round-trip **identically** | all three ✅ |
| 3 | `funcref` | `funcref_probe.wat` -- `ref.func` plus a declarative elem | both exports exist when the module instantiates | all three ✅ |
| 4 | tail calls, register form | `tailcall_probe.wat` -- `(i32)->i32` via `return_call` | the call returns the argument it was given | all three ✅ |
| 5 | tail calls, frame form | `tailcall_frame_probe.wat` -- three `v128` + `return_call` | the call returns lane 0 of the first vector | SLJIT ⛔ · AsmJIT ⛔ · LibJIT **n/a** |

Each probe prints one line per backend with a `PROBE`/`EXC`/`REF`/`TAILFRAME` prefix, so a
red run **names the backend** without a debugger.

## 2. Shape of a probe (copy this)

```cpp
TEST_CASE("<feature>: refuse or answer correctly", "[staging][abi][<tag>]")
{
    for (const char* backend : {"libjit", "asmjit", "sljit"}) {
        std::fprintf(stderr, "PROBE %s: ", backend);
        auto runtime = WASM::Runtime::create(backend);
        auto module = runtime->loadFile(path / "<fixture>.wasm");
        WASM::ModuleInstance* instance = nullptr;
        try {
            instance = &runtime->instantiate(*module, imports);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "REFUSED: %s\n", error.what());
            continue;                      // refusal is a correct answer
        }
        /* ...call through the right form; assert the CORRECT answer... */
    }
}
```

Four rules learned the hard way, all in this file because each cost a round:

1. **Refusal is a pass**, not a skip -- `continue` after printing, never `SUCCEED`.
2. **Assert the property, not the outcome.** A probe that asserts "it works" cannot
   distinguish a working backend from a buggy one that happens to return 0.
3. **Test data must be inert** -- a fabricated pointer (`0x1234`) as an `externref` was
   retained in host-handle bookkeeping and broke an unrelated test (section 18 of the
   gaps note). Use `nullptr`.
4. **Run the whole suite before committing**, with a fixed seed: `--rng-seed 12345`.
   A filtered green run says the probe works; only the suite says the probe is safe.

## 3. What a probe is NOT

A probe is not a spec test. It does not try to cover a feature; it establishes **which of
three states a backend is in**, because those three states have different consequences for
an embedder:

| State | Consequence |
|---|---|
| works | the embedder can use it |
| refuses | the embedder learns immediately, at instantiation, with a message |
| **silent wrong answer** | the embedder finds out in production |

Only the third is a defect, and only a probe can find it -- the tree is *silent* there.

## 4. Probes 6 and 7, measured

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 6 | GC struct (`struct.new`) | ✅ instantiate + export | ✅ instantiate + export | ✅ instantiate + export |
| 7 | atomics (`i32.atomic.rmw.add` on shared memory) | ⛔ refuses | ⛔ refuses | ✅ |

### What each probe actually proves, and no more

**Probe 6 is the weakest in the set and is labelled as such**: it asserts that the module
instantiates and the export exists. It does **not** call `make`, so it establishes that
the *boundary and declaration* accept a struct type -- not that `struct.new` produces a
usable object. A stronger version calls the export and inspects the reference; that needs
a GC-aware assertion, and is the natural next step for this row.

**Probe 7 is clean**: LibJIT exposes `bump`; the other two refuse with
*"opcode dispatch is not implemented: dispatchI32AtomicRmwAdd"* (AsmJIT) and the same in
SLJIT's spelling. Two refusals and one acceptance -- the first row where **LibJIT leads**,
and no silent wrong answer anywhere.

### The matrix after seven probes

| Feature | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|
| `v128`, multi-result, calls, imports | per PRODUCTION_GAPS §21 | | |
| exceptions | ⛔ | ⛔ | ❌ |
| `externref`, `funcref` | ✅ | ✅ | ✅ |
| tail calls (register) | ✅ | ✅ | ✅ |
| tail calls (frame) | ⛔ | ⛔ | n/a |
| **GC struct declarations** | ✅ (weak) | ✅ (weak) | ✅ (weak) |
| **atomics** | ⛔ | ⛔ | ✅ |
| memory64 (beyond bulk) | unprobed | unprobed | unprobed |

## 5. Probes 6 (strengthened) and 8: GC structs and memory64 both work everywhere

```
GC  libjit: struct ref non-null      M64 libjit: pages=1
GC  asmjit: struct ref non-null      M64 asmjit: pages=1
GC  sljit:  struct ref non-null      M64 sljit:  pages=1
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 6 | GC struct, **called** | ✅ returns a reference | ✅ | ✅ |
| 8 | memory64 (`memory i64`, `memory.size`) | ✅ 1 page | ✅ 1 page | ✅ 1 page |

### Probe 6 is no longer the weak row

The previous version only proved that a module *declaring* a struct instantiated. Calling
`make` and requiring a **non-null reference** proves `struct.new` produced a real object
on every backend -- which is the difference between "accepted" and "works" that section 4
of this note is about.

### memory64 was the last unprobed row, and it passes

`(memory i64 1)` plus `memory.size` answering **1** on all three backends. Note this does
not contradict SLJIT's documented bulk-memory refusal (`PRODUCTION_GAPS` §15's
`memory64_bulk` fixture): that fixture uses bulk-memory instructions, which SLJIT does not
lower; the *memory itself* is fine. Two different questions, two different answers, both
recorded.

### The matrix after eight probes

| Feature | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|
| `v128`, multi-result, calls, imports | per PRODUCTION_GAPS §21 | | |
| **exceptions** | ⛔ | ⛔ | ❌ |
| `externref`, `funcref` | ✅ | ✅ | ✅ |
| tail calls (register / frame) | ✅ / ⛔ | ✅ / ⛔ | ✅ / n/a |
| GC struct (`struct.new`, called) | ✅ | ✅ | ✅ |
| **atomics** | ⛔ | ⛔ | ✅ |
| memory64 (memory itself) | ✅ | ✅ | ✅ |
| memory64 + bulk memory | ⛔ | unprobed | unprobed |

**Every row now has a measured answer.** The only ❌ in the matrix remains LibJIT's silent
`throw`, and the only ⛔ rows are honest refusals with reasons.

Suite 21916 assertions in 784 cases under `--rng-seed 12345`, green.

## 6. Probes 9 and 10: GC arrays work everywhere; memory64 + bulk memory does not

```
GCARR    libjit/asmjit/sljit: array ref non-null
M64BULK  libjit: instantiated
M64BULK  asmjit: REFUSED: AsmJit does not lower bulk memory on memory64 yet
M64BULK  sljit:  REFUSED: does not lower bulk memory on memory64 yet
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 9 | GC array (`array.new`, called) | ✅ reference | ✅ | ✅ |
| 10 | memory64 **with bulk memory** | ⛔ refuses | ⛔ refuses | ✅ |

### This completes the memory64 picture, and it is a two-row story

* **memory itself** (`memory i64`, `memory.size`): ✅ on all three (probe 8);
* **memory64 + bulk memory**: ⛔ on SLJIT and AsmJIT, ✅ on LibJIT.

Both rows are needed: a single "memory64" row would have been wrong in whichever direction
it was filled in. The refusal message is also precise -- *"does not lower bulk memory on
memory64 YET"* -- which reads as a scheduled limitation rather than a design boundary.

### GC arrays pass the same test structs passed

`array.new` returns a **non-null reference** on all three backends. Together with probe 6
that makes two GC constructors (`struct.new`, `array.new`) measured as *working*, not
merely *accepted*.

### Probe 10 was already a test -- in the wrong place

SLJIT's `memory64_bulk` refusal had a test of its own (`[sljit][m64][refusal]`), and it
encodes the current policy correctly. Probe 10 duplicates it deliberately: the *point* of
a probe is that it runs the **same fixture on every backend**, so the matrix row is
measured rather than assembled from three differently-shaped tests.

## 7. Probes 11 and 12: bulk memory on memory32 and multi-memory both work

```
PROBE bulk32_probe      on libjit/asmjit/sljit: export=1  copied
PROBE multimemory_probe on libjit/asmjit/sljit: export=1  memory1=1
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 11 | bulk memory on a memory32 (`memory.copy`) | ✅ called | ✅ | ✅ |
| 12 | multi-memory (`memory.size 1`) | ✅ 1 page | ✅ | ✅ |

**Both were strengthened before being believed.** The first version only checked that the
exports existed -- the weakness probe 6 was called out for. The final version **calls**
`copy` (a zero-length copy cannot fault, but the lowering has to exist) and asserts that
the **second** memory reports **1** page.

That detail matters for the row: `memory.size 1` reads an *indexed* memory, so a backend
that quietly ignored the index would answer 0 or crash. It answers 1 on all three.
