;; SIMD-L3: integer arithmetic, comparisons, shifts, reductions and the
;; width-changing family, end to end through the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; Observed one lane at a time — or as the reduction's own i32 result, which needs
;; no marshalling at all, because the harness still cannot move a `v128` (gap S7).
;;
;; The operands are chosen so that each hazard shows up as a wrong *number* rather
;; than as a coincidence: `-128` against `1` separates the signed from the
;; unsigned forms, `1..16` makes a vector's lower and upper halves tell each other
;; apart (so `extend_low`/`extend_high` and `narrow`'s two halves cannot be
;; swapped), and shift counts of 8/33/64 land in the "must be masked" range.
(module
  ;; ── shared operands ──────────────────────────────────────────────────────
  ;; `$min8` is -128 as i8 and 128 as u8: the value where the two readings differ.
  (func $min8 (result v128) (i8x16.splat (i32.const -128)))
  (func $one8 (result v128) (i8x16.splat (i32.const 1)))
  (func $all8 (result v128)
    (v128.const i8x16 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
  (func $min16 (result v128) (i16x8.splat (i32.const -32768)))
  (func $max16 (result v128) (i16x8.splat (i32.const 32767)))
  (func $half16 (result v128) (i16x8.splat (i32.const 16384)))
  (func $hundred8 (result v128) (i8x16.splat (i32.const 100)))
  (func $p200 (result v128) (i8x16.splat (i32.const -56)))       ;; 0xC8 as u8

  ;; ── comparisons: signed and unsigned disagree on the same operands ───────
  (func (export "lt_s_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.lt_s (call $min8) (call $one8))))     ;; 0xff
  (func (export "lt_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.lt_u (call $min8) (call $one8))))     ;; 0x00
  (func (export "gt_s_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.gt_s (call $min8) (call $one8))))     ;; 0x00
  (func (export "gt_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.gt_u (call $min8) (call $one8))))     ;; 0xff
  (func (export "le_s_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.le_s (call $min8) (call $one8))))     ;; 0xff
  (func (export "ge_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.ge_u (call $min8) (call $one8))))     ;; 0xff
  (func (export "eq_i32_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.eq (v128.const i32x4 7 7 7 7) (v128.const i32x4 7 7 7 7)))) ;; -1, i.e. all ones
  (func (export "ne_i32_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.ne (v128.const i32x4 7 7 7 7) (v128.const i32x4 8 8 8 8)))) ;; -1
  ;; i64x2 has no unsigned order comparison, so this one must be signed.
  (func (export "i64_lt_s_lane0") (result i64)
    (i64x2.extract_lane 0
      (i64x2.lt_s (v128.const i64x2 -1 -1) (v128.const i64x2 1 1))))     ;; -1

  ;; ── min/max: the same distinction ───────────────────────────────────────
  (func (export "min_s_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.min_s (call $min8) (call $one8))))    ;; 0x80
  (func (export "min_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.min_u (call $min8) (call $one8))))    ;; 0x01
  (func (export "max_s_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.max_s (call $min8) (call $one8))))    ;; 0x01
  (func (export "max_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.max_u (call $min8) (call $one8))))    ;; 0x80

  ;; ── add/sub/mul, neg/abs: wrapping two's complement ─────────────────────
  (func (export "add_wrap_lane0") (result i32)
    (i8x16.extract_lane_s 0
      (i8x16.add (v128.const i8x16 127 127 127 127 127 127 127 127
                              127 127 127 127 127 127 127 127)
                 (call $one8))))                                         ;; -128, not 128
  (func (export "sub_wrap_lane0") (result i32)
    (i8x16.extract_lane_s 0
      (i8x16.sub (i8x16.splat (i32.const 0)) (call $one8))))             ;; -1
  (func (export "neg_min_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.neg (call $min8))))                   ;; 0x80
  (func (export "abs_min_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.abs (call $min8))))                   ;; 0x80
  (func (export "mul_i32_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.mul (v128.const i32x4 7 7 7 7) (v128.const i32x4 -3 -3 -3 -3))))  ;; -21
  (func (export "add_i64_lane0") (result i64)
    (i64x2.extract_lane 0
      (i64x2.add (v128.const i64x2 1 1) (v128.const i64x2 -2 -2))))      ;; -1

  ;; ── shifts: the count is taken modulo the lane width ────────────────────
  (func (export "shl_masked_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.shl (v128.const i32x4 7 7 7 7) (i32.const 33))))            ;; 33 mod 32 == 1 -> 14
  (func (export "shl_i8_masked_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.shl (call $one8) (i32.const 8))))     ;; 8 mod 8 == 0 -> 1
  (func (export "shr_u_i64_masked_lane0") (result i64)
    (i64x2.extract_lane 0
      (i64x2.shr_u (v128.const i64x2 1 1) (i32.const 64))))              ;; 64 mod 64 == 0 -> 1
  (func (export "shr_s_neg_lane0") (result i32)
    (i8x16.extract_lane_s 0 (i8x16.shr_s (call $min8) (i32.const 1))))   ;; -64
  (func (export "shr_u_neg_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.shr_u (call $min8) (i32.const 1))))   ;; 0x40
  (func (export "shl_i16_lane7") (result i32)
    (i16x8.extract_lane_u 7 (i16x8.shl (call $one8) (i32.const 15))))    ;; 0x8000

  ;; ── saturating add/sub, and the rounding average ────────────────────────
  (func (export "add_sat_s_lane0") (result i32)
    (i8x16.extract_lane_s 0
      (i8x16.add_sat_s (call $hundred8) (call $hundred8))))              ;; 127
  (func (export "add_sat_u_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.add_sat_u (call $p200) (call $hundred8))))                  ;; 0xff
  (func (export "sub_sat_s_lane0") (result i32)
    (i8x16.extract_lane_s 0 (i8x16.sub_sat_s (call $min8) (call $one8))))      ;; -128
  (func (export "sub_sat_u_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.sub_sat_u (call $one8) (call $min8))))      ;; 0
  (func (export "avgr_u_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.avgr_u (call $one8) (i8x16.splat (i32.const 2)))))                ;; (1+2+1)/2 = 2
  (func (export "avgr_u_sat_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.avgr_u (i8x16.splat (i32.const -1)) (i8x16.splat (i32.const -1)))))  ;; 255

  ;; ── popcnt, and q15mulr_sat_s's one overflowing product (hazard Y5) ─────
  (func (export "popcnt_full_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.popcnt (i8x16.splat (i32.const -1)))))                        ;; 8
  (func (export "popcnt_nibble_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.popcnt (i8x16.splat (i32.const 15)))))                        ;; 4
  (func (export "q15_sat_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.q15mulr_sat_s (call $min16) (call $min16))))                  ;; 32767
  (func (export "q15_round_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.q15mulr_sat_s (call $min16) (call $max16))))                  ;; -32767
  (func (export "q15_quarter_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.q15mulr_sat_s (call $half16) (call $half16))))                ;; 8192

  ;; ── reductions: the i32 results need no marshalling at all ──────────────
  (func (export "all_true_ones") (result i32) (i8x16.all_true (call $one8)))     ;; 1
  (func (export "all_true_zero") (result i32)
    (i8x16.all_true (i8x16.splat (i32.const 0))))                                ;; 0
  (func (export "all_true_i16") (result i32) (i16x8.all_true (call $one8)))      ;; 1
  (func (export "all_true_i64") (result i32) (i64x2.all_true (call $one8)))      ;; 1
  (func (export "any_true_zero") (result i32)
    (v128.any_true (i8x16.splat (i32.const 0))))                                 ;; 0
  (func (export "any_true_last_byte") (result i32)
    (v128.any_true
      (i8x16.replace_lane 15 (i8x16.splat (i32.const 0)) (i32.const 1))))        ;; 1
  (func (export "bitmask_lane0") (result i32)
    (i8x16.bitmask
      (i8x16.replace_lane 0 (i8x16.splat (i32.const 0)) (i32.const -128))))      ;; 0x0001
  (func (export "bitmask_lane15") (result i32)
    (i8x16.bitmask
      (i8x16.replace_lane 15 (i8x16.splat (i32.const 0)) (i32.const -128))))     ;; 0x8000
  (func (export "bitmask_i64_lane1") (result i32)
    (i64x2.bitmask
      (i64x2.replace_lane 1 (i64x2.splat (i64.const 0)) (i64.const -1))))        ;; 0x2

  ;; ── the width-changing family: the two halves must not be swapped ───────
  (func (export "extend_low_lane0") (result i32)
    (i16x8.extract_lane_s 0 (i16x8.extend_low_i8x16_s (call $all8))))     ;; 1
  (func (export "extend_high_lane0") (result i32)
    (i16x8.extract_lane_s 0 (i16x8.extend_high_i8x16_s (call $all8))))    ;; 9
  (func (export "extmul_low_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.extmul_low_i8x16_s (call $all8) (call $all8))))              ;; 1 * 1
  (func (export "extmul_high_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.extmul_high_i8x16_s (call $all8) (call $all8))))             ;; 9 * 9 = 81
  (func (export "extmul_u_lane0") (result i32)
    (i16x8.extract_lane_u 0
      (i16x8.extmul_low_i8x16_u (call $min8) (call $min8))))              ;; 128 * 128 = 16384
  (func (export "extadd_pairwise_lane0") (result i32)
    (i16x8.extract_lane_s 0
      (i16x8.extadd_pairwise_i8x16_s (call $all8))))                      ;; 1 + 2 = 3
  (func (export "dot_wrap_lane0") (result i32)
    (i32x4.extract_lane 0
      (i32x4.dot_i16x8_s (call $min16) (call $min16))))                   ;; 2 * 2^30 wraps
  (func (export "narrow_s_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.narrow_i16x8_s (i16x8.splat (i32.const 300))
                            (i16x8.splat (i32.const -300)))))             ;; 300 clamps to 0x7f
  (func (export "narrow_s_lane8") (result i32)
    (i8x16.extract_lane_u 8
      (i8x16.narrow_i16x8_s (i16x8.splat (i32.const 300))
                            (i16x8.splat (i32.const -300)))))             ;; -300 clamps to 0x80
  (func (export "narrow_u_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (i8x16.narrow_i16x8_u (i16x8.splat (i32.const 300))
                            (i16x8.splat (i32.const -300)))))             ;; 0xff
  (func (export "narrow_u_lane8") (result i32)
    (i8x16.extract_lane_u 8
      (i8x16.narrow_i16x8_u (i16x8.splat (i32.const 300))
                            (i16x8.splat (i32.const -300)))))             ;; 0x00

  ;; A chain, so an intermediate really crosses a slot boundary:
  ;; max_s(min_u(-128, 1), add_sat_s(100, 100)) lane 0 = max_s(1, 127) = 127.
  (func (export "chain_lane0") (result i32)
    (i8x16.extract_lane_s 0
      (i8x16.max_s (i8x16.min_u (call $min8) (call $one8))
                   (i8x16.add_sat_s (call $hundred8) (call $hundred8)))))
)
