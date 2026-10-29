# The ABI baselines

`libWasmBase.exports` and `libLibJit.exports` are the **linker-visible ABI** of
the two libraries at the last accepted state: one `symbol TYPE` line per strong
dynamic export (`T`, `D`, `B`, `R`), sorted. Weak template instantiations are
excluded on purpose — they follow the compiler, not the contract.

`Test/abi-check.sh <build-dir>` compares a build against them:

| Result | Meaning |
|---|---|
| removed symbols | **failure** — an embedder built against the previous release would stop linking (docs/API_ABI_STABILITY.md G2: additive-only evolution) |
| added symbols | reported, not a failure; regenerate the baseline in the same change that adds them |

Regenerate after an **intentional** additive change:

```sh
./Test/abi-check.sh --update build/<config>
```

## Why this and not only `abidiff`

The check is deliberately dependency-free: `nm` is in binutils and every build
host has it. If libabigail is installed, `ABI_REFERENCE=<previous
libWasmBase.so> ./Test/abi-check.sh <build-dir>` adds a full structural
`abidiff` on top; without libabigail the symbol baseline still catches the
regression that matters for a frozen ABI — a symbol disappearing.

## Scope, and the known superset (`AAS12`)

The frozen surface is S1–S10 in `API_ABI_STABILITY.md` §2.2: the calling
convention, the POD layouts, the GC layout, the trap path, the continuation
provider, the helper ABI, the instantiator/session seam, the invocation path,
the diagnostic schema and the plugin descriptor. The export list is **larger**
than that surface: `[tiers]`-internal headers such as the component model and
the validator are compiled with default visibility today, because public headers
include them from inside their `WASMJIT_VISIBLE_BEGIN` region and because
`GcHeap`/`Section` are reachable from embedder-tier types.

So this baseline is the *conservative* contract — everything an embedder can
link against may not vanish — and `AAS12` records the tightening options
(pin the internal headers to hidden and promote the type-translation surface to
the backend tier, or accept the superset explicitly). Until that decision, the
check is additive-only over the superset, which is strictly safer than not
checking at all.
