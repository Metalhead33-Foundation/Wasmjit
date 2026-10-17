;; SIMD-L1: `v128.const`, `*.splat`, extract/replace lane — lane semantics end to
;; end through the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; The byte pattern used throughout is the one hazard Y11 is about:
;;   `v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d`
;; is, in memory, the 16 bytes 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 —
;; so i8x16 lane 0 is 0x01 and lane 15 is 0x10, i32x4 lane 0 is 0x04030201 and
;; lane 3 is 0x100f0e0d, and i64x2 lane 0 is 0x0807060504030201.
(module
  (func (export "const_i32_lane0") (result i32)
    (i32x4.extract_lane 0
      (v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d)))
  (func (export "const_i32_lane3") (result i32)
    (i32x4.extract_lane 3
      (v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d)))
  (func (export "const_u8_lane0") (result i32)
    (i8x16.extract_lane_u 0
      (v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d)))
  (func (export "const_u8_lane15") (result i32)
    (i8x16.extract_lane_u 15
      (v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d)))
  (func (export "const_i64_lane0") (result i64)
    (i64x2.extract_lane 0
      (v128.const i32x4 0x04030201 0x08070605 0x0c0b0a09 0x100f0e0d)))

  ;; splat fills every lane with the scalar.
  (func (export "splat_i8_lane0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.splat (i32.const 0x5A))))
  (func (export "splat_i8_lane15") (result i32)
    (i8x16.extract_lane_u 15 (i8x16.splat (i32.const 0x5A))))
  (func (export "splat_i16_lane7") (result i32)
    (i16x8.extract_lane_u 7 (i16x8.splat (i32.const 0x1234))))
  (func (export "splat_i32_lane2") (result i32)
    (i32x4.extract_lane 2 (i32x4.splat (i32.const -7))))
  (func (export "splat_i64_lane1") (result i64)
    (i64x2.extract_lane 1 (i64x2.splat (i64.const -2))))

  ;; Signed vs unsigned extraction of the same lane.
  (func (export "splat_i8_signed") (result i32)
    (i8x16.extract_lane_s 3 (i8x16.splat (i32.const 0xFF))))
  (func (export "splat_i8_unsigned") (result i32)
    (i8x16.extract_lane_u 3 (i8x16.splat (i32.const 0xFF))))
  (func (export "splat_i16_signed") (result i32)
    (i16x8.extract_lane_s 0 (i16x8.splat (i32.const 0x8000))))

  ;; replace_lane changes exactly one lane and leaves the rest alone.
  (func (export "replace_i32_lane1") (result i32)
    (i32x4.extract_lane 1
      (i32x4.replace_lane 1
        (v128.const i32x4 0 0 0 0)
        (i32.const 0x0BADF00D))))
  (func (export "replace_i32_untouched") (result i32)
    (i32x4.extract_lane 0
      (i32x4.replace_lane 1
        (v128.const i32x4 0 0 0 0)
        (i32.const 0x0BADF00D))))
  (func (export "replace_i8_lane15") (result i32)
    (i8x16.extract_lane_u 15
      (i8x16.replace_lane 15
        (v128.const i32x4 0 0 0 0)
        (i32.const 0xAB))))

  ;; A computed vector: const -> replace -> extract, so the value really flows
  ;; through the slot rather than being folded away.
  (func (export "const_replace_extract") (result i32)
    (i32x4.extract_lane 0
      (i32x4.replace_lane 0
        (v128.const i32x4 0 0 0 0)
        (i32.const 0xFEEDFACE))))
)
