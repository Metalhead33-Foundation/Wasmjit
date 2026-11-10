# Backend notes {#backends_index}

*Per-backend slices, defects, measurements and plans*

Status: index, revision 1 (2026-11-10). The notes here are the working record of
the three code generators; the **verdict** is owned by
[`BACKEND_PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md), and the
per-feature ledgers by [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md).

Audience: anyone reviewing a backend change, planning the next slice, or asking
why a shape is refused on one backend and not another. Read the row that owns the
question, not the whole folder.

This folder is deliberately not a second status page. A note here is one of three
kinds, and the kind decides how its numbers may be read:

- **Current ledgers** — updated with the tree; they own the per-backend facts.
- **Process notes** — the ABI design, its consequences and the open work.
- **Historical records** — a dated post-mortem or after-action report. Its numbers
  are a snapshot of that pass and are never a statement about today; current
  numbers live in the current ledgers.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | Which note owns the current per-backend state? |
| §3 | Which note owns a design or process question? |
| §4 | Which notes are historical records? |
| §5 | Where are the backend pages and the ABI baseline? |
| §6 | References |

## 2. Current ledgers

| Note | What it owns |
|---|---|
| [BACKEND_PRODUCTION_READINESS.md](../BACKEND_PRODUCTION_READINESS.md) | The per-backend production verdict, the full-suite measurement and the promotion gate. Start here for "is it ready?". |
| \subpage backend_production_gaps | The curated capability ledger (§27) and the production-gap ledger (§29–§30), backend by backend. |
| \subpage backend_capability_probes | The probe instruments that fill the matrix: one fixture, one assertion, three recorded outcomes. |
| \subpage backend_call_form_granularity | The register-form versus frame-form decision, its measured cost and the per-signature recommendation. |

## 3. Design and process notes

| Note | What it owns |
|---|---|
| \subpage backend_staging_call_abi | The opt-in frame ABI: `void fn(VMContext*, void* frame)`, its layout, the migration log and the defects found. |
| \subpage backend_export_signature | What the frame form changes for imports, exports and compiled-to-compiled calls; the embedder seam keeps typed entry points. |
| \subpage backend_v128_signature_decision | The canonical `v128` boundary: two `uint64_t` words, the rejected alternatives, and the per-target reasoning. |
| \subpage backend_gc_plan | The GC-construction plan for SLJIT and AsmJit and the proof it was written around. |
| \subpage backend_libjit_argument_defect | The measured LibJIT `r9` loss that motivated the frame ABI. |
| \subpage backend_handoff | The session handoff and the sweep defect list. |

## 4. Historical records

These are dated post-mortems. They explain how a slice was built and what broke;
they do **not** describe the current tree.

| Note | What it records |
|---|---|
| \subpage backend_p0_p2 | The P0–P2 parity slice: SLJIT brought up to AsmJit's execution surface. |
| \subpage backend_p2_memory64 | The memory64 slice and its residual refusals. |
| \subpage backend_p3_float | The scalar `f32`/`f64` slice. |
| \subpage backend_p3_gc1 | GC construction slice 1 (`struct.new`, `struct.get`). |
| \subpage backend_p3_refs1 | Reference slice 1 (`ref.null`, `ref.is_null`, `ref.eq`). |
| \subpage backend_p3_simd1 | v128 slice 1: transport, bitwise, `i32x4` lanes. |
| \subpage backend_p3_simd2 | v128 slice 2: integer lanes, comparisons, `all_true`. |
| \subpage backend_p3_simd3 | v128 slice 3: shifts, saturation, `min`/`max`, `abs`, `popcnt`. |
| \subpage backend_p3_simd4 | v128 slice 4: widening, `extmul`, `extadd_pairwise`, `dot`, `narrow`. |
| \subpage backend_p3_simd5 | v128 slice 5: the quarantined AsmJit `narrow` defect. |
| \subpage backend_p3_simd6 | v128 slice 6: float lanes. |
| \subpage backend_p3_simd7 | v128 slice 7: float lane access. |
| \subpage backend_p3_simd8 | v128 slice 8: vector transport and swizzle. |
| \subpage backend_p3_simd9 | v128 slice 9: `i8x16.shuffle`. |
| \subpage backend_post_mortem | The remaining-work post-mortem for the 2026-11-07 pass. |

## 5. The backends themselves

| Page | What it contains |
|---|---|
| \subpage backend_asmjit | `Asmjit/Asmjit.md`: build dependency, emitted surface, refusals and next steps. |
| \subpage backend_sljit | `Sljit/Sljit.md`: vendored SLJIT, emitted surface, refusals and next steps. |
| [ABI baselines](../../Test/abi/ABI_BASELINES.md) | The link-visible ABI baselines and `abi-check.sh` (indexed under the `docs` node). |

## 6. References

- [`BACKEND_PRODUCTION_READINESS.md`](../BACKEND_PRODUCTION_READINESS.md) — the
  current verdict and the measured full-suite runs.
- [`WRITING_A_BACKEND.md`](../WRITING_A_BACKEND.md) — the checklist a new code
  generator must satisfy, with `Sljit/` as the worked example.
- [`LAYERING.md`](../LAYERING.md) — the rule that keeps `WasmBase/` free of
  backend names.
- [`PRODUCTION_GAPS.md`](PRODUCTION_GAPS.md) §30 — the defect ledger that closed
  the last production blockers.
- [`../index.md`](../index.md) — the documentation tree this folder is a child of.
