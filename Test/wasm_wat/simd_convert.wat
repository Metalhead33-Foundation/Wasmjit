;; SIMD-L4 (conversions): the float↔int conversions, end to end through
;; the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; Every export projects a *bit pattern*, and the operands are the ends of the
;; ranges rather than ordinary numbers: ±2^31 and 2^32 for the saturating forms,
;; a NaN, and a value whose fractional part must be truncated. `*_zero` operands
;; carry a marker in the lanes that must come out zero, so an implementation
;; that fills them from the operand cannot pass.
(module
  (func (export "trunc_sat_s_nan") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_s (v128.const f32x4 nan nan nan nan))))
  (func (export "trunc_sat_s_pos") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_s (v128.const f32x4 2147483648 2147483648 2147483648 2147483648))))
  (func (export "trunc_sat_s_neg") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_s (v128.const f32x4 -2147483904 -2147483904 -2147483904 -2147483904))))
  (func (export "trunc_sat_s_frac") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_s (v128.const f32x4 -1.9 -1.9 -1.9 -1.9))))
  (func (export "trunc_sat_u_neg") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_u (v128.const f32x4 -1.5 -1.5 -1.5 -1.5))))
  (func (export "trunc_sat_u_big") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_u (v128.const f32x4 4294967296 4294967296 4294967296 4294967296))))
  (func (export "trunc_sat_u_near") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f32x4_u (v128.const f32x4 4294967040 4294967040 4294967040 4294967040))))
  (func (export "convert_s_neg") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0
        (f32x4.convert_i32x4_s (v128.const i32x4 -1 -1 -1 -1)))))
  (func (export "convert_u_max") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0
        (f32x4.convert_i32x4_u (v128.const i32x4 -1 -1 -1 -1)))))
  (func (export "convert_low_s_lane1") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 1
        (f64x2.convert_low_i32x4_s (v128.const i32x4 0 -2147483648 7 7)))))
  (func (export "convert_low_u_lane0") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 0
        (f64x2.convert_low_i32x4_u (v128.const i32x4 -1 5 7 7)))))
  (func (export "promote_low_lane1") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 1
        (f64x2.promote_low_f32x4 (v128.const f32x4 1.5 -0 7 7)))))
  (func (export "demote_zero_lane0") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0
        (f32x4.demote_f64x2_zero (v128.const f64x2 1.5 -2.5)))))
  (func (export "demote_zero_lane2") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 2
        (f32x4.demote_f64x2_zero (v128.const f64x2 1.5 -2.5)))))
  (func (export "trunc_sat_f64_s_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f64x2_s_zero (v128.const f64x2 -2147483649 -5.5))))
  (func (export "trunc_sat_f64_s_lane1") (result i32)
    (i32x4.extract_lane 1
      (i32x4.trunc_sat_f64x2_s_zero (v128.const f64x2 -2147483649 -5.5))))
  (func (export "trunc_sat_f64_s_lane2") (result i32)
    (i32x4.extract_lane 2
      (i32x4.trunc_sat_f64x2_s_zero (v128.const f64x2 -2147483649 -5.5))))
  (func (export "trunc_sat_f64_u_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.trunc_sat_f64x2_u_zero (v128.const f64x2 -1 4294967296))))
  (func (export "trunc_sat_f64_u_lane1") (result i32)
    (i32x4.extract_lane 1
      (i32x4.trunc_sat_f64x2_u_zero (v128.const f64x2 -1 4294967296))))
)
