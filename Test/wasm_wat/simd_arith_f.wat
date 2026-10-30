;; SIMD-L4 (float half): float arithmetic, comparisons and rounding, end to end
;; through the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; Every export projects a *bit pattern* (i32/i64) rather than a float, so nothing
;; has to cross the harness boundary as a float — and so that a NaN's payload and a
;; zero's sign stay visible to the assertions.
;;
;; The cases are the ones the hardware and the library get wrong rather than the
;; ones that are easy to get right: `ne` on a NaN, `min`/`max` on `+0`/`-0` and on
;; a NaN (hazard Y1), and `nearest`'s ties-to-even.
(module
  (func $nan (result v128) (v128.const f32x4 nan nan nan nan))
  (func $one (result v128) (v128.const f32x4 1 1 1 1))
  (func $two (result v128) (v128.const f32x4 2 2 2 2))
  (func $pos0 (result v128) (v128.const f32x4 0 0 0 0))
  (func $neg0 (result v128) (v128.const f32x4 -0 -0 -0 -0))

  ;; ── comparisons: the mask, as a bitmask ─────────────────────────────────
  ;; `ne` is *unordered*: a NaN operand makes it true, where the other five are
  ;; false. All four lanes set gives 0xffff.
  (func (export "cmp_ne_nan") (result i32)
    (i8x16.bitmask (f32x4.ne (call $nan) (call $one))))            ;; 0xffff
  (func (export "cmp_eq_nan") (result i32)
    (i8x16.bitmask (f32x4.eq (call $nan) (call $nan))))            ;; 0x0000
  (func (export "cmp_lt_nan") (result i32)
    (i8x16.bitmask (f32x4.lt (call $nan) (call $one))))            ;; 0x0000
  (func (export "cmp_lt_ok") (result i32)
    (i8x16.bitmask (f32x4.lt (call $one) (call $two))))            ;; 0xffff
  (func (export "cmp_ge_ok") (result i32)
    (i8x16.bitmask (f32x4.ge (call $one) (call $two))))            ;; 0x0000
  (func (export "cmp_ge_eq") (result i32)
    (i8x16.bitmask (f32x4.ge (call $one) (call $one))))            ;; 0xffff

  ;; ── arithmetic, as bit patterns ─────────────────────────────────────────
  (func (export "add_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.add (call $one) (call $two))))) ;; 3.0
  (func (export "sub_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.sub (call $one) (call $two))))) ;; -1.0
  (func (export "mul_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.mul (call $one) (call $two))))) ;; 2.0
  (func (export "div_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.div (call $one) (call $two))))) ;; 0.5
  (func (export "sqrt_bits") (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.sqrt (v128.const f32x4 4 4 4 4)))))                    ;; 2.0
  (func (export "abs_bits") (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.abs (v128.const f32x4 -1.5 -1.5 -1.5 -1.5)))))        ;; 1.5
  (func (export "neg_bits") (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.neg (v128.const f32x4 1.5 1.5 1.5 1.5)))))            ;; -1.5

  ;; ── min/max: the corners the instructions get wrong (Y1) ────────────────
  (func (export "min_zero_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.min (call $pos0) (call $neg0))))) ;; -0
  (func (export "max_zero_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.max (call $pos0) (call $neg0))))) ;; +0
  (func (export "min_nan_exponent") (result i32)
    (i32.and (i32.shr_u
      (i32.reinterpret_f32
        (f32x4.extract_lane 0 (f32x4.min (call $nan) (call $one))))
      (i32.const 23)) (i32.const 0xff)))                            ;; 0xff: still a NaN
  (func (export "max_nan_exponent") (result i32)
    (i32.and (i32.shr_u
      (i32.reinterpret_f32
        (f32x4.extract_lane 0 (f32x4.max (call $one) (call $nan))))
      (i32.const 23)) (i32.const 0xff)))                            ;; 0xff
  ;; `pmin`/`pmax` do not propagate a NaN: they fall through to `a`.
  (func (export "pmin_nan_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.pmin (call $nan) (call $one))))) ;; the NaN itself
  (func (export "pmax_nan_bits") (result i32)
    (i32.reinterpret_f32
      (f32x4.extract_lane 0 (f32x4.pmax (call $one) (call $nan))))) ;; 1.0

  ;; ── rounding: `nearest` ties to even, the rest are directed ──────────────
  (func $near (param $f f32) (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.nearest (f32x4.splat (local.get $f))))))
  (func $ceil1 (param $f f32) (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.ceil (f32x4.splat (local.get $f))))))
  (func $floor1 (param $f f32) (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.floor (f32x4.splat (local.get $f))))))
  (func $trunc1 (param $f f32) (result i32)
    (i32.reinterpret_f32 (f32x4.extract_lane 0
      (f32x4.trunc (f32x4.splat (local.get $f))))))
  (func (export "nearest_2_5") (result i32) (call $near (f32.const 2.5)))    ;; 2.0
  (func (export "nearest_3_5") (result i32) (call $near (f32.const 3.5)))    ;; 4.0
  (func (export "nearest_m2_5") (result i32) (call $near (f32.const -2.5)))  ;; -2.0
  (func (export "nearest_m3_5") (result i32) (call $near (f32.const -3.5)))  ;; -4.0
  (func (export "nearest_m0_5") (result i32) (call $near (f32.const -0.5)))  ;; -0.0
  (func (export "nearest_2_4") (result i32) (call $near (f32.const 2.4)))    ;; 2.0
  (func (export "nearest_inf") (result i32) (call $near (f32.const inf)))    ;; +inf
  (func (export "ceil_m1_5") (result i32) (call $ceil1 (f32.const -1.5)))    ;; -1.0
  (func (export "floor_m1_5") (result i32) (call $floor1 (f32.const -1.5)))  ;; -2.0
  (func (export "trunc_m1_5") (result i32) (call $trunc1 (f32.const -1.5)))  ;; -1.0

  ;; ── f64x2: the same three families ──────────────────────────────────────
  (func (export "f64_add_bits") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 0
        (f64x2.add (v128.const f64x2 1 1) (v128.const f64x2 2 2)))))
  (func (export "f64_min_zero_bits") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 0
        (f64x2.min (v128.const f64x2 0 0) (v128.const f64x2 -0 -0)))))
  (func (export "f64_nearest_bits") (result i64)
    (i64.reinterpret_f64
      (f64x2.extract_lane 0
        (f64x2.nearest (v128.const f64x2 2.5 2.5)))))
  (func (export "f64_cmp_masks") (result i32)
    (i8x16.bitmask
      (f64x2.lt (v128.const f64x2 1 1) (v128.const f64x2 2 2))))
  (func (export "f64_ne_nan") (result i32)
    (i8x16.bitmask
      (f64x2.ne (v128.const f64x2 nan nan) (v128.const f64x2 1 1))))
)
