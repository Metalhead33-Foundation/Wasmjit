# Backend notes {#backends_index}

*Where the current backend facts live, and the one historical record*

Status: index, revision 2 (2026-11-10). The per-slice reports, the capability
probes, the production-gap ledger and the handoff were consolidated: **current**
facts live in the documents below, and **history** lives in
[`HISTORY.md`](HISTORY.md). This folder deliberately has only one note.

## 1. Current backend facts

| Topic | Read |
|---|---|
| Production verdict, measurement envelope, promotion gate | [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §13 |
| **Every open defect, gap and limitation**, plus the closed index and the decisions that are not defects | [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8 |
| The canonical signature, `v128` boundary, register/frame call forms, cross-backend policy | [`../ABI.md`](../ABI.md) |
| The target matrix, GC object ABI and what a backend may not do | [`../ABI.md`](../ABI.md) |
| SIMD helper ABI, per-backend lowering, relaxed-SIMD rules and measured cost | [`../proposals/SIMD.md`](../proposals/SIMD.md) §4.5–§4.10 |
| Capability probes, required runs and the promotion-gate commands | [`../TESTING.md`](../TESTING.md) |
| The checklist and walkthrough for a code generator, with SLJIT as the worked example | [`../WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) |
| The rule that keeps `WasmBase/` free of backend names | [`../LAYERING.md`](../LAYERING.md) |
| Link-visible ABI baselines and `abi-check.sh` | [`../../Test/abi/ABI_BASELINES.md`](../../Test/abi/ABI_BASELINES.md) |

## 2. The backends themselves

| Page | What it contains |
|---|---|
| \subpage backend_asmjit | `Asmjit/Asmjit.md`: build dependency, emitted surface, refusals and next steps. |
| \subpage backend_sljit | `Sljit/Sljit.md`: vendored SLJIT, emitted surface, refusals and next steps. |

## 3. History

| Note | What it records |
|---|---|
| \subpage backend_history | The merged, frozen record: `v128` decision, frame-ABI migration, every AsmJit/SLJIT slice, the defect log and the process lessons. It is **not** a statement about today. |

## 4. References

- [`../index.md`](../index.md) — the documentation tree this folder is a child of.
- [`../API_ABI_STABILITY.md`](../API_ABI_STABILITY.md) §8/§13 — the current
  register and verdict.
- [`HISTORY.md`](HISTORY.md) — the dated record.
- Git history — the deleted per-slice reports, verbatim, for any detail the
  history note omits.
