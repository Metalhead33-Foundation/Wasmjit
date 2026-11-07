;; The ninth shared v128 slice: i8x16.shuffle, the only five-argument helper in
;; the SIMD ABI. Results are observed through i32x4 lanes.
(module
  (func (export "shuffle_interleave") (result i32)
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 0 17 2 19 4 21 6 23 8 25 10 27 12 29 14 31
    i32x4.extract_lane 0)
  (func (export "shuffle_all_zero") (result i32)
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    i32x4.extract_lane 0)
  (func (export "shuffle_take_b") (result i32)
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
    i32x4.extract_lane 0)
  (func (export "shuffle_reverse") (result i32)
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
    i32x4.extract_lane 0)
  (func (export "shuffle_twice") (result i32)
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
    drop
    v128.const i32x4 0x04030201 0x08070605 0x0C0B0A09 0x100F0E0D
    v128.const i32x4 0x14131211 0x18171615 0x1C1B1A19 0x201F1E1D
    i8x16.shuffle 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
    i32x4.extract_lane 0)
)
