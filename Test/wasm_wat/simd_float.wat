;; The sixth shared v128 slice: float lane arithmetic, comparisons, unary
;; operations and conversions. Results are observed as raw IEEE bit patterns
;; through i32x4 lanes, so no v128 or float signature is needed.
(module
  (func (export "f32x4_add") (result i32)
    v128.const f32x4 1.5 0 0 0
    v128.const f32x4 2.5 0 0 0
    f32x4.add
    i32x4.extract_lane 0)
  (func (export "f32x4_sub") (result i32)
    v128.const f32x4 5.5 0 0 0
    v128.const f32x4 2.25 0 0 0
    f32x4.sub
    i32x4.extract_lane 0)
  (func (export "f32x4_mul") (result i32)
    v128.const f32x4 1.5 0 0 0
    v128.const f32x4 4 0 0 0
    f32x4.mul
    i32x4.extract_lane 0)
  (func (export "f32x4_div") (result i32)
    v128.const f32x4 7.5 0 0 0
    v128.const f32x4 2.5 0 0 0
    f32x4.div
    i32x4.extract_lane 0)
  (func (export "f32x4_min") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.min
    i32x4.extract_lane 0)
  (func (export "f32x4_max") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.max
    i32x4.extract_lane 0)
  (func (export "f32x4_pmin") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.pmin
    i32x4.extract_lane 0)
  (func (export "f32x4_pmax") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.pmax
    i32x4.extract_lane 0)
  (func (export "f64x2_add") (result i32)
    v128.const f64x2 1.5 0
    v128.const f64x2 2.5 0
    f64x2.add
    i32x4.extract_lane 1)
  (func (export "f64x2_mul") (result i32)
    v128.const f64x2 1.5 0
    v128.const f64x2 4 0
    f64x2.mul
    i32x4.extract_lane 1)
  (func (export "f64x2_min") (result i32)
    v128.const f64x2 1 0
    v128.const f64x2 2 0
    f64x2.min
    i32x4.extract_lane 1)
  (func (export "f64x2_max") (result i32)
    v128.const f64x2 1 0
    v128.const f64x2 2 0
    f64x2.max
    i32x4.extract_lane 1)
  (func (export "f32x4_sqrt") (result i32)
    v128.const f32x4 9 0 0 0
    f32x4.sqrt
    i32x4.extract_lane 0)
  (func (export "f32x4_abs") (result i32)
    v128.const f32x4 -2.5 0 0 0
    f32x4.abs
    i32x4.extract_lane 0)
  (func (export "f32x4_neg") (result i32)
    v128.const f32x4 2.5 0 0 0
    f32x4.neg
    i32x4.extract_lane 0)
  (func (export "f32x4_ceil") (result i32)
    v128.const f32x4 1.25 0 0 0
    f32x4.ceil
    i32x4.extract_lane 0)
  (func (export "f32x4_floor") (result i32)
    v128.const f32x4 -1.25 0 0 0
    f32x4.floor
    i32x4.extract_lane 0)
  (func (export "f32x4_trunc") (result i32)
    v128.const f32x4 -1.75 0 0 0
    f32x4.trunc
    i32x4.extract_lane 0)
  (func (export "f32x4_nearest") (result i32)
    v128.const f32x4 2.5 0 0 0
    f32x4.nearest
    i32x4.extract_lane 0)
  (func (export "f64x2_sqrt") (result i32)
    v128.const f64x2 9 0
    f64x2.sqrt
    i32x4.extract_lane 1)
  (func (export "f64x2_abs") (result i32)
    v128.const f64x2 -2.5 0
    f64x2.abs
    i32x4.extract_lane 1)
  (func (export "f32x4_eq") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 1 0 0 0
    f32x4.eq
    i32x4.extract_lane 0)
  (func (export "f32x4_ne") (result i32)
    v128.const f32x4 nan 0 0 0
    v128.const f32x4 nan 0 0 0
    f32x4.ne
    i32x4.extract_lane 0)
  (func (export "f32x4_lt") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.lt
    i32x4.extract_lane 0)
  (func (export "f32x4_ge") (result i32)
    v128.const f32x4 1 0 0 0
    v128.const f32x4 2 0 0 0
    f32x4.ge
    i32x4.extract_lane 0)
  (func (export "f64x2_lt") (result i32)
    v128.const f64x2 1 0
    v128.const f64x2 2 0
    f64x2.lt
    i32x4.extract_lane 0)
  (func (export "i32x4_trunc_sat_f32x4_s") (result i32)
    v128.const f32x4 3.9 0 0 0
    i32x4.trunc_sat_f32x4_s
    i32x4.extract_lane 0)
  (func (export "i32x4_trunc_sat_f32x4_u") (result i32)
    v128.const f32x4 -1 0 0 0
    i32x4.trunc_sat_f32x4_u
    i32x4.extract_lane 0)
  (func (export "f32x4_convert_i32x4_s") (result i32)
    v128.const i32x4 3 0 0 0
    f32x4.convert_i32x4_s
    i32x4.extract_lane 0)
  (func (export "f64x2_convert_low_i32x4_s") (result i32)
    v128.const i32x4 3 0 0 0
    f64x2.convert_low_i32x4_s
    i32x4.extract_lane 1)
  (func (export "f64x2_promote_low_f32x4") (result i32)
    v128.const f32x4 2 0 0 0
    f64x2.promote_low_f32x4
    i32x4.extract_lane 1)
  (func (export "f32x4_demote_f64x2_zero") (result i32)
    v128.const f64x2 2 0
    f32x4.demote_f64x2_zero
    i32x4.extract_lane 0)
  (func (export "i32x4_trunc_sat_f64x2_s_zero") (result i32)
    v128.const f64x2 3.9 0
    i32x4.trunc_sat_f64x2_s_zero
    i32x4.extract_lane 0)
)
