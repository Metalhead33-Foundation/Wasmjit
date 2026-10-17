;; Relaxed SIMD (SIMD-5 / docs/proposals/SIMD.md SD5): one export per hand-written
;; entry point, so the JIT path is exercised for the shapes the op list cannot
;; express (ternary madd/laneselect, the byte swizzle and the pairwise dots).
;; The strict cases — relaxed_trunc, relaxed_min/max, relaxed_q15mulr — are
;; op-list entries and are covered by the helper differential instead.
(module
  ;; a = 10..25, b = indices 2,0,0,... -> out[0] = a[2] = 12 (the zeroing reading).
  (func (export "relaxed_swizzle_lane0") (result i32)
    v128.const i8x16 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25
    v128.const i8x16 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    i8x16.relaxed_swizzle
    i8x16.extract_lane_s 0)

  ;; all-ones mask in lane 0 picks a (1), all-zeros picks b (2).
  (func (export "relaxed_laneselect_lane0") (result i32)
    v128.const i32x4 1 1 1 1
    v128.const i32x4 2 2 2 2
    v128.const i32x4 -1 0 0 0
    i32x4.relaxed_laneselect
    i32x4.extract_lane 0)

  ;; a*b + c and c - a*b, unfused (the permitted reading SD5 picks).
  (func (export "relaxed_madd_lane0") (result f32)
    v128.const f32x4 2 0 0 0
    v128.const f32x4 3 0 0 0
    v128.const f32x4 1 0 0 0
    f32x4.relaxed_madd
    f32x4.extract_lane 0)             ;; 2*3 + 1 = 7

  (func (export "relaxed_nmadd_lane0") (result f32)
    v128.const f32x4 2 0 0 0
    v128.const f32x4 3 0 0 0
    v128.const f32x4 1 0 0 0
    f32x4.relaxed_nmadd
    f32x4.extract_lane 0)             ;; 1 - 2*3 = -5

  ;; i8 x u8 adjacent products with the *saturating* pair sum (SD5's reading).
  ;; Pairs: 1*2 + 3*4 = 14, 100*100 + (-100)*100 = 0, 100*100 + 100*100 = 20000,
  ;; then 127*255 + 127*255 = 64770 — past i16, so 32767 is the answer that tells
  ;; the saturating reading apart from a wrapping one. `b`'s 255 also has its high
  ;; bit set, exercising the "may be read unsigned" case the spec leaves open.
  (func (export "relaxed_dot_lane2") (result i32)
    v128.const i8x16 1 3 100 -100 100 100 127 127 0 0 0 0 0 0 0 0
    v128.const i8x16 2 4 100 100 100 100 -1 -1 0 0 0 0 0 0 0 0
    i16x8.relaxed_dot_i8x16_i7x16_s
    i16x8.extract_lane_s 2)           ;; 20000

  (func (export "relaxed_dot_sat_lane3") (result i32)
    v128.const i8x16 1 3 100 -100 100 100 127 127 0 0 0 0 0 0 0 0
    v128.const i8x16 2 4 100 100 100 100 -1 -1 0 0 0 0 0 0 0 0
    i16x8.relaxed_dot_i8x16_i7x16_s
    i16x8.extract_lane_s 3)           ;; 64770 saturates to 32767

  ;; The same pairs folded into i32 and accumulated: (1*2+3*4) + (2*3+4*5) + 100
  ;; = 14 + 26 + 100 = 140.
  (func (export "relaxed_dot_add_lane0") (result i32)
    v128.const i8x16 1 3 2 4 0 0 0 0 0 0 0 0 0 0 0 0
    v128.const i8x16 2 4 3 5 0 0 0 0 0 0 0 0 0 0 0 0
    v128.const i32x4 100 0 0 0
    i32x4.relaxed_dot_i8x16_i7x16_add_s
    i32x4.extract_lane 0)
)
