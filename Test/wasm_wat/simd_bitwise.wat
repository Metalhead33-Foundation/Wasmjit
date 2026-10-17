;; SIMD-L2: the bitwise ops end to end through the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; Observed one lane at a time, because the harness cannot marshal a `v128` yet
;; (gap S7). `a` deliberately has four *distinct* lanes so a lane-permuting bug
;; cannot hide, and `andnot`/`bitselect` have golden values that pin which
;; operand is complemented / which one selects.
;;
;;   a = 0x0F0F0F0F 0x11111111 0x22222222 0x33333333
;;   b = 0x00FF00FF 0x00FF00FF 0x00FF00FF 0x00FF00FF
;;   c = 0xFFFF0000 0xFFFF0000 0xFFFF0000 0xFFFF0000
(module
  (func $a (result v128) (v128.const i32x4 0x0F0F0F0F 0x11111111 0x22222222 0x33333333))
  (func $b (result v128) (v128.const i32x4 0x00FF00FF 0x00FF00FF 0x00FF00FF 0x00FF00FF))
  (func $c (result v128) (v128.const i32x4 0xFFFF0000 0xFFFF0000 0xFFFF0000 0xFFFF0000))

  ;; v128.not: ~0x0F0F0F0F = 0xF0F0F0F0, ~0x33333333 = 0xCCCCCCCC
  (func (export "not_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.not (call $a))))
  (func (export "not_lane3") (result i32)
    (i32x4.extract_lane 3 (v128.not (call $a))))

  ;; v128.and / or / xor against b
  (func (export "and_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.and (call $a) (call $b))))
  (func (export "and_lane3") (result i32)
    (i32x4.extract_lane 3 (v128.and (call $a) (call $b))))
  (func (export "or_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.or (call $a) (call $b))))
  (func (export "xor_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.xor (call $a) (call $b))))

  ;; v128.andnot (a, b) is a AND NOT b: 0x0F0F0F0F & ~0x00FF00FF = 0x0F000F00
  (func (export "andnot_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.andnot (call $a) (call $b))))
  (func (export "andnot_lane3") (result i32)
    (i32x4.extract_lane 3 (v128.andnot (call $a) (call $b))))

  ;; v128.bitselect (a, b, c) is (a & c) | (b & ~c); c selects
  (func (export "bitselect_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.bitselect (call $a) (call $b) (call $c))))
  (func (export "bitselect_lane3") (result i32)
    (i32x4.extract_lane 3 (v128.bitselect (call $a) (call $b) (call $c))))

  ;; A computed chain, so the intermediate really flows through a slot:
  ;; not(and(a, b)) lane 0 = ~0x000F000F = 0xFFF0FFF0
  (func (export "chain_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.not (v128.and (call $a) (call $b))))))
