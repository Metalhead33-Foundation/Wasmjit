;; The shared scalar floating-point fixture for the AsmJit and SLJIT backends.
;; Every family the P3 slice claims is exported once, so one conformance test
;; can drive both generators over the same module.
(module
  (memory 1 2)
  (type $mixed (func (param i32 f64) (result f64)))
  (table 2 funcref)
  (global $g32 (export "g32") (mut f32) (f32.const 1.5))
  (global $g64 (export "g64") (mut f64) (f64.const 2.25))

  (func $mixed_f64 (type $mixed)
    local.get 1
    f64.const 2
    f64.mul)
  (elem (i32.const 0) $mixed_f64)

  ;; ── arithmetic ───────────────────────────────────────────────────────────
  (func (export "f32_add") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.add)
  (func (export "f32_sub") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.sub)
  (func (export "f32_mul") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.mul)
  (func (export "f32_div") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.div)
  (func (export "f32_min") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.min)
  (func (export "f32_max") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.max)
  (func (export "f32_copysign") (param f32 f32) (result f32)
    local.get 0 local.get 1 f32.copysign)
  (func (export "f64_add") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.add)
  (func (export "f64_sub") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.sub)
  (func (export "f64_mul") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.mul)
  (func (export "f64_div") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.div)
  (func (export "f64_min") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.min)
  (func (export "f64_max") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.max)
  (func (export "f64_copysign") (param f64 f64) (result f64)
    local.get 0 local.get 1 f64.copysign)

  ;; ── unary ────────────────────────────────────────────────────────────────
  (func (export "f32_abs") (param f32) (result f32) local.get 0 f32.abs)
  (func (export "f32_neg") (param f32) (result f32) local.get 0 f32.neg)
  (func (export "f32_sqrt") (param f32) (result f32) local.get 0 f32.sqrt)
  (func (export "f32_ceil") (param f32) (result f32) local.get 0 f32.ceil)
  (func (export "f32_floor") (param f32) (result f32) local.get 0 f32.floor)
  (func (export "f32_trunc") (param f32) (result f32) local.get 0 f32.trunc)
  (func (export "f32_nearest") (param f32) (result f32) local.get 0 f32.nearest)
  (func (export "f64_abs") (param f64) (result f64) local.get 0 f64.abs)
  (func (export "f64_neg") (param f64) (result f64) local.get 0 f64.neg)
  (func (export "f64_sqrt") (param f64) (result f64) local.get 0 f64.sqrt)
  (func (export "f64_ceil") (param f64) (result f64) local.get 0 f64.ceil)
  (func (export "f64_floor") (param f64) (result f64) local.get 0 f64.floor)
  (func (export "f64_trunc") (param f64) (result f64) local.get 0 f64.trunc)
  (func (export "f64_nearest") (param f64) (result f64) local.get 0 f64.nearest)

  ;; ── constants and locals ─────────────────────────────────────────────────
  (func (export "f32_const") (result f32) f32.const 3.5)
  (func (export "f64_const") (result f64) f64.const -7.25)
  (func (export "f32_local") (param f32) (result f32) (local f32)
    local.get 0 local.set 1 local.get 1)
  (func (export "f64_local") (param f64) (result f64) (local f64)
    local.get 0 local.set 1 local.get 1)

  ;; ── comparisons ──────────────────────────────────────────────────────────
  (func (export "f32_eq") (param f32 f32) (result i32) local.get 0 local.get 1 f32.eq)
  (func (export "f32_ne") (param f32 f32) (result i32) local.get 0 local.get 1 f32.ne)
  (func (export "f32_lt") (param f32 f32) (result i32) local.get 0 local.get 1 f32.lt)
  (func (export "f32_gt") (param f32 f32) (result i32) local.get 0 local.get 1 f32.gt)
  (func (export "f32_le") (param f32 f32) (result i32) local.get 0 local.get 1 f32.le)
  (func (export "f32_ge") (param f32 f32) (result i32) local.get 0 local.get 1 f32.ge)
  (func (export "f64_eq") (param f64 f64) (result i32) local.get 0 local.get 1 f64.eq)
  (func (export "f64_ne") (param f64 f64) (result i32) local.get 0 local.get 1 f64.ne)
  (func (export "f64_lt") (param f64 f64) (result i32) local.get 0 local.get 1 f64.lt)
  (func (export "f64_gt") (param f64 f64) (result i32) local.get 0 local.get 1 f64.gt)
  (func (export "f64_le") (param f64 f64) (result i32) local.get 0 local.get 1 f64.le)
  (func (export "f64_ge") (param f64 f64) (result i32) local.get 0 local.get 1 f64.ge)

  ;; ── globals ──────────────────────────────────────────────────────────────
  (func (export "f32_get_global") (result f32) global.get $g32)
  (func (export "f32_set_global") (param f32) (result f32)
    local.get 0 global.set $g32 global.get $g32)
  (func (export "f64_get_global") (result f64) global.get $g64)
  (func (export "f64_set_global") (param f64) (result f64)
    local.get 0 global.set $g64 global.get $g64)

  ;; ── memory ───────────────────────────────────────────────────────────────
  (func (export "f32_store_load") (param i32 f32) (result f32)
    local.get 0 local.get 1 f32.store
    local.get 0 f32.load)
  (func (export "f64_store_load") (param i32 f64) (result f64)
    local.get 0 local.get 1 f64.store
    local.get 0 f64.load)
  (func (export "f32_load_oob") (param i32) (result f32) local.get 0 f32.load)
  (func (export "f64_load_oob") (param i32) (result f64) local.get 0 f64.load)

  ;; ── conversions ──────────────────────────────────────────────────────────
  (func (export "i32_trunc_f32_s") (param f32) (result i32) local.get 0 i32.trunc_f32_s)
  (func (export "i32_trunc_f32_u") (param f32) (result i32) local.get 0 i32.trunc_f32_u)
  (func (export "i32_trunc_f64_s") (param f64) (result i32) local.get 0 i32.trunc_f64_s)
  (func (export "i32_trunc_f64_u") (param f64) (result i32) local.get 0 i32.trunc_f64_u)
  (func (export "i64_trunc_f32_s") (param f32) (result i64) local.get 0 i64.trunc_f32_s)
  (func (export "i64_trunc_f32_u") (param f32) (result i64) local.get 0 i64.trunc_f32_u)
  (func (export "i64_trunc_f64_s") (param f64) (result i64) local.get 0 i64.trunc_f64_s)
  (func (export "i64_trunc_f64_u") (param f64) (result i64) local.get 0 i64.trunc_f64_u)
  (func (export "i32_trunc_sat_f32_s") (param f32) (result i32) local.get 0 i32.trunc_sat_f32_s)
  (func (export "i32_trunc_sat_f32_u") (param f32) (result i32) local.get 0 i32.trunc_sat_f32_u)
  (func (export "i32_trunc_sat_f64_s") (param f64) (result i32) local.get 0 i32.trunc_sat_f64_s)
  (func (export "i32_trunc_sat_f64_u") (param f64) (result i32) local.get 0 i32.trunc_sat_f64_u)
  (func (export "i64_trunc_sat_f32_s") (param f32) (result i64) local.get 0 i64.trunc_sat_f32_s)
  (func (export "i64_trunc_sat_f32_u") (param f32) (result i64) local.get 0 i64.trunc_sat_f32_u)
  (func (export "i64_trunc_sat_f64_s") (param f64) (result i64) local.get 0 i64.trunc_sat_f64_s)
  (func (export "i64_trunc_sat_f64_u") (param f64) (result i64) local.get 0 i64.trunc_sat_f64_u)
  (func (export "f32_convert_i32_s") (param i32) (result f32) local.get 0 f32.convert_i32_s)
  (func (export "f32_convert_i32_u") (param i32) (result f32) local.get 0 f32.convert_i32_u)
  (func (export "f32_convert_i64_s") (param i64) (result f32) local.get 0 f32.convert_i64_s)
  (func (export "f32_convert_i64_u") (param i64) (result f32) local.get 0 f32.convert_i64_u)
  (func (export "f64_convert_i32_s") (param i32) (result f64) local.get 0 f64.convert_i32_s)
  (func (export "f64_convert_i32_u") (param i32) (result f64) local.get 0 f64.convert_i32_u)
  (func (export "f64_convert_i64_s") (param i64) (result f64) local.get 0 f64.convert_i64_s)
  (func (export "f64_convert_i64_u") (param i64) (result f64) local.get 0 f64.convert_i64_u)
  (func (export "f32_demote_f64") (param f64) (result f32) local.get 0 f32.demote_f64)
  (func (export "f64_promote_f32") (param f32) (result f64) local.get 0 f64.promote_f32)

  ;; ── reinterpretation ─────────────────────────────────────────────────────
  (func (export "i32_reinterpret_f32") (param f32) (result i32) local.get 0 i32.reinterpret_f32)
  (func (export "f32_reinterpret_i32") (param i32) (result f32) local.get 0 f32.reinterpret_i32)
  (func (export "i64_reinterpret_f64") (param f64) (result i64) local.get 0 i64.reinterpret_f64)
  (func (export "f64_reinterpret_i64") (param i64) (result f64) local.get 0 f64.reinterpret_i64)

  ;; ── select and mixed-ABI calls ───────────────────────────────────────────
  (func (export "f32_select") (param f32 f32 i32) (result f32)
    local.get 0 local.get 1 local.get 2 select)
  (func (export "f64_select") (param f64 f64 i32) (result f64)
    local.get 0 local.get 1 local.get 2 select)
  (func (export "f64_select_t") (param f64 f64 i32) (result f64)
    local.get 0 local.get 1 local.get 2 select (result f64))

  ;; ── float structured results ─────────────────────────────────────────────
  (func (export "f32_block_result") (result f32)
    block (result f32)
      f32.const 1.25
    end)
  (func (export "f64_if_result") (param i32) (result f64)
    local.get 0
    if (result f64)
      f64.const 1.5
    else
      f64.const 2.5
    end)
  (func (export "f32_br_result") (result f32)
    block (result f32)
      f32.const 3.5
      br 0
    end)
  (func (export "call_mixed") (param i32 f64) (result f64)
    local.get 0 local.get 1 call $mixed_f64)
  (func (export "call_indirect_mixed") (param i32 f64 i32) (result f64)
    local.get 0 local.get 1 local.get 2 call_indirect (type $mixed))
)
