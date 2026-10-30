# `S3` — AssemblyScript and TypeScript {#lf2_s3}

Status: `LF-2` cluster report (revision 1 — 2026-10-30). Cluster `L3` of
[`CLUSTERING.md`](CLUSTERING.md) §3.1; rows `T15` and `T30` of
[`TRIAGE.md`](TRIAGE.md) §5.2/§5.4.

| Row | Arm | Verdict |
|---|---|---|
| `T15` | AssemblyScript 0.28.19, `asc --runtime stub --optimize` | **Green** |
| `T30` | TypeScript via AssemblyScript | **Green**, by inheritance from `T15` — no separate arm exists |

Audience: anyone embedding a TypeScript-shaped workload, and `LF-3` when it
decides whether the cluster already counts as answered.

## 1. Why this cluster is one arm

`T30` is not a toolchain: the `LF-1` screen recorded that TypeScript's only
documented WebAssembly path is AssemblyScript, and the row folds into `T15`
([`TRIAGE.md`](TRIAGE.md) §5.6, the "folds into" marker). A separate TypeScript
study would re-measure the same compiler with different source syntax.

The cluster also *starts* with evidence: `Test/wasm/*.ts` is built at test time
by the same `asc`, and `string_interop.ts` / `native_interop.ts` already exercise
string, array and cached-handle marshalling against a native import
([`TESTING.md`](../TESTING.md) Part 1). The charter told `LF-2` to consume that
evidence, not to re-buy it
([`LANGUAGE_FEASIBILITY.md`](../LANGUAGE_FEASIBILITY.md) §7.6). This report
therefore adds only what the harness does not: the module's import set, its
memory shape, and a `Callable` round-trip built the same way as the other arms.

## 2. The arm

```
asc index.ts -o t15_as.wasm --optimize --runtime stub --noAssert --initialMemory 1
```

`t15_as.wasm`: 144 bytes, 1 import (`env.host_add`), 5 exports, one memory of
1 page. The `stub` runtime is the right choice for a minimal module: it has no
allocator and no GC, so the 144 bytes are the whole guest. A real application
would use `--runtime incremental` (the default) and bring its own TLSF heap and
incremental collector, which is still guest-side — nothing in either runtime
needs a host service.

```
call answer/i32() -> i32=42
call add/i32(19,23) -> i32=42
call viaHost/i32(19,23) -> i32=42      # host_add_calls=1
memory memory: 65536 bytes (1 pages) max=4294967296 shared=0
```

`stringLen` is exported but not called through the probe: a `string` parameter
in AssemblyScript is a pointer into linear memory, and passing one requires the
runner to write UTF-16 into the guest. The harness already covers that direction
with a real native import (`native_interop.ts`), so the gap is in this report's
transcript, not in the evidence base.

## 3. The criteria table

| Criterion | `T15` |
|---|---|
| Export through `Callable` | yes |
| Native host import | yes (`env.host_add`), and strings/arrays in the harness |
| Second module's export | not run; mechanism proven on `T02` |
| Single-agent contract | yes |
| Engine patch | no |
| WASI imports | 0 |
| Heap owner | the guest (TLSF in `incremental`, none in `stub`) |
| Declared maximum | 4 GiB |
| `Reserved` usable | yes |

## 4. Memory layout and start-up

One memory, index 0, one page initially, 4 GiB maximum, grown by the runtime's
own allocator. No start function; `__wasm_call_ctors` does not exist in a
`stub` build. With the default `incremental` runtime, the heap is still a guest
allocation inside memory 0, so the embedder's decision is the same as `T02`'s:
`Reserved` if the module caches raw pointers, `Growable` otherwise.

## 5. What this cluster leaves open

- A full `incremental`-runtime module (GC + TLSF) was not built here because the
  harness already builds those modules on every test run; a `C2` verdict does
  not need a second copy.
- The probe cannot marshal a `string` argument, so this report does not
  demonstrate `stringLen` itself. The harness does.
- `T30` has no independent evidence, by definition. If a non-AssemblyScript
  TypeScript compiler appears with a standalone target, its row re-enters
  `LF-1`, not this report.
