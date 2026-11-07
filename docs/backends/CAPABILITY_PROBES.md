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

## 8. Probes 13 and 14: exception handlers work on LibJIT; GC array ops are D-2 and inconclusive

### Probe 13 -- exception HANDLERS

```
EXCH libjit: caught()=42
EXCH asmjit: REFUSED: opcode dispatch is not implemented: dispatchTryTable
EXCH sljit:  REFUSED: does not lower dispatchTryTable
```

| SLJIT | AsmJIT | LibJIT |
|---|---|---|
| ⛔ refuses | ⛔ refuses | **✅ handler runs, returns 42** |

This sharpens D-1 considerably: **LibJIT's exception support is real, not absent.** The
handler path works -- a `throw` inside a `try_table` reaches the catch and the function
returns the payload. What D-1 records is narrower than "exceptions are broken":

> **LibJIT handles a caught `throw` correctly and silently returns from an UNCAUGHT one.**

So the defect is in the *uncaught* path: a throw with no handler must trap (pending-status)
and instead the function returns `0`. That is a much smaller and better-aimed bug than the
matrix suggested, and finding it took a second fixture.

### Probe 14 -- GC array operations: D-2, and a probe that is still inconclusive

```
GCARROP libjit: SKIPPED -- known abort (D-2)
GCARROP asmjit: REFUSED: out of bounds array access
GCARROP sljit:  REFUSED: out of bounds array access
```

**D-2 (LibJIT):** `array.set` / `array.fill` **abort the process** --
`Assertion '!valueStack.empty()' failed` in `LibjitOpcodeDispatcher.cpp:1783`. An abort is
worse than a trap: it cannot be caught, so the probe skips LibJIT entirely and says so.

**The AsmJIT/SLJIT result is a RUNTIME TRAP, not a refusal**, and it is reported as such to
be honest about what is known: the module instantiated, then the call trapped with *out of
bounds array access*. Two explanations fit, and the probe cannot yet tell them apart:

1. **my fixture's operand order is wrong** for `array.set` / `array.fill` (two earlier
   versions of this fixture were invalid for exactly this reason -- a missing `array.fill`
   count, then a `get` on a consumed array); or
2. the backends pop `array.set` operands in a different order than the spec's
   `(array, index, value)`, making index and value swap -- which would make a legitimate
   access out of bounds.

**Next step, not a conclusion:** cross-check the fixture against the spec text and the
reference interpreter before recording anything as a defect. The third version of this
fixture is the one that would tell us; the first two measured the probe, not the feature.

### The meta-lesson, twice in one probe

A probe that is *invalid wasm* makes every backend look broken -- and the messages
(*operand stack underflow*, *consumes more operands than it produces*) sound like backend
bugs when they are correct rejections. **The fixture must be validated before its results
are believed**, which is the same rule as "inert test data" and "assert the property",
applied one step earlier.

## 9. Probes 15 and 16: tables work everywhere; a reference in a struct field is D-3

```
TABLE    libjit/asmjit/sljit: via_table(3,4)=7
REFFIELD libjit: roundtrip(null)=null
REFFIELD asmjit: roundtrip(null)=null
REFFIELD sljit:  roundtrip(null)=non-null     <- D-3
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 15 | `funcref` in a table + `call_indirect` | ✅ 7 | ✅ 7 | ✅ 7 |
| 16 | `externref` stored in a struct field | ❌ **D-3** | ✅ null | ✅ null |

### D-3: SLJIT mangles a reference that passes through a struct field

An identity round-trip of `null` returns **non-null** on SLJIT. The function is
`(externref) -> externref` and its body is `struct.new` then `struct.get` of field 0, so
the reference must come back unchanged -- and on two backends it does.

The likely cause is a *boxing* mistake: SLJIT probably returns the **GC object**
(the struct it just built) instead of the **payload read out of its field**. That would
explain a non-null value that is nevertheless a valid pointer, and it is worth checking
first when this is fixed.

Pinned, not tolerated: the test asserts SLJIT's **current wrong** behaviour, so closing
the defect forces the test to be rewritten.

## 10. Q-1 was my fixture, not the backends

`array.fill` has spec coverage in the tree (`spec: array`, `spec: array_fill`,
`spec: array_new_elem`), and all three pass. Combined with the two earlier invalid
versions of the probe fixture, the honest conclusion is:

> **Q-1 does not become a defect.** My `gc_array_ops_probe` fixture is the likely
> culprit; the feature has passing spec coverage in the shapes the suite exercises.

**D-2 stands independently of Q-1**: LibJIT's abort was reproduced with an assertion in
`popValueRaw`, and the probe skips LibJIT so the suite survives. D-2 still needs a
minimal reproduction that the spec tests do not already cover -- which is a task for the
fix session, not for probing.

## 11. Probes 17 and 18: `array.get` works on both non-LibJIT backends; threads stay LibJIT-only

```
ARRGET  asmjit: probe()=22        THREADS libjit:  store_load()=123
ARRGET  sljit:  probe()=22        THREADS asmjit:  REFUSED: dispatchI32AtomicStore
                                  THREADS sljit:   REFUSED: does not lower dispatchI32AtomicStore
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 17 | `array.new_fixed` + `array.get` | ✅ 22 | ✅ 22 | **skipped** (D-2) |
| 18 | atomic store + load on shared memory | ⛔ refuses | ⛔ refuses | ✅ 123 |

### Probe 17 closes Q-1 in the direction the spec tests suggested

`array.new_fixed` + `array.get` returns **22** on both SLJIT and AsmJIT. Combined with the
passing `spec: array*` tests and my two invalid fixtures, the conclusion is settled: the
earlier "out of bounds array access" was **my fixture**, not the backends.

LibJIT is **skipped, not passed**, and labelled so: D-2 makes its GC array path capable of
aborting the process, and an abort cannot be caught. The LibJIT cell is therefore
*unknown* -- the only honest label.

### Probe 18 keeps threads LibJIT-only, now with two instructions of evidence

A single `atomic.rmw.add` (probe 7) and a `store`/`load` pair both work on LibJIT and are
both refused by SLJIT and AsmJIT, with messages naming the missing dispatch. So the
refusal is consistent across the atomics family rather than an accident of one opcode.

## 12. The sweep is complete except for two areas

| Area | Why it is still open |
|---|---|
| GC arrays with **reference** elements | only `i32` element arrays probed; a `(array (mut externref))` is a different lowering path |
| exception payloads **across instances** | the handler probe is single-module; a tag thrown in one instance and caught in another exercises the instance-boundary rule |

Everything else in the matrix now has a measured answer, a refusal with a reason, or an
explicit `skipped`/`unknown` label.

## 13. Probes 19 and 20: reference arrays work (including SLJIT), tag boundaries resolve correctly

```
REFARR    asmjit: roundtrip(null)=null      TAGEXPORT libjit/asmjit/sljit: exported=1
REFARR    sljit:  roundtrip(null)=null      TAGIMPORT libjit/asmjit/sljit: REFUSED: Unresolved import: provider.e
```

| # | Probe | SLJIT | AsmJIT | LibJIT |
|---|---|---|---|---|
| 19 | array of `externref` elements | ✅ null | ✅ null | skipped (D-2) |
| 20a | a module **exports** a tag | ✅ | ✅ | ✅ |
| 20b | a module **imports** a tag (unregistered) | ⛔ unresolved | ⛔ unresolved | ⛔ unresolved |

### D-3 is narrower than it looked, and this probe says so

My first version of probe 19 **assumed** the struct-field defect would repeat for arrays and
asserted SLJIT's *wrong* behaviour. It returned **null** -- correctly -- so the assumption
was wrong and the assertion was changed to require the correct answer.

> **D-3 is specific to STRUCT fields.** A reference round-trips correctly through an
> *array* on SLJIT. The defect is in one lowering path, not in SLJIT's reference handling
> as a whole.

That is a materially better fix target than "SLJIT mangles references", and it was one
assertion away from being recorded wrongly.

### Probe 20: the tag boundary is *resolution*, not lowering

All three backends accept a module that **exports** a tag, and all three refuse a module
that **imports** one when no resolver provides it -- with the same message,
*"Unresolved import: provider.e"*. So the tag's `ExternalKind` is understood by the
boundary machinery on every backend; what is unproven is whether a tag actually thrown in
one instance is caught in another, which needs a registered provider and two live
instances. That is a larger fixture than the sweep budgeted for, and it is recorded as the
remaining open question rather than half-tested.

## 14. Sweep closed

Nineteen probes over thirteen sections. Every matrix cell now carries one of: measured ✅,
refused ⛔ with a reason, `skipped` (with why), or `unknown` (with what would resolve it).

The one area left genuinely open is **a tag thrown in one instance and caught in another**,
which needs a two-instance fixture. Everything else in `PRODUCTION_GAPS.md` §24's ledger
stands as measured.

## 15. Probe 21: cross-instance exception payloads WORK on LibJIT

```
XINST libjit: caught()=7
XINST asmjit: REFUSED: opcode dispatch is not implemented: dispatchThrow
XINST sljit:  REFUSED: does not lower dispatchThrow
```

A provider instance exports a tag and throws it (`i32.const 7; throw $e`); a **separate**
consumer instance imports both the tag and the throwing function and catches it. LibJIT
returns **7** -- the payload crosses the instance boundary intact.

| SLJIT | AsmJIT | LibJIT |
|---|---|---|
| ⛔ refuses | ⛔ refuses | **✅ 7** |

This was the last open question of the sweep, and it closes the exception picture in a way
that makes D-1 sharper still: **LibJIT's exception implementation is substantially
complete** -- tags cross instances, handlers catch, payloads arrive -- and the defect is
confined to the *uncaught* path, where a throw returns instead of leaving a pending trap.
