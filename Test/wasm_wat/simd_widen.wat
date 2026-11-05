;; The fourth shared v128 slice: widening, extmul, extadd_pairwise, narrowing
;; and dot. Results are observed through i32x4 lanes, so the module still needs
;; no v128 signature.
(module
  (func (export "i16x8_extend_low_i8x16_s") (result i32)
    v128.const i32x4 0x01010101 0x02020202 0 0
    i16x8.extend_low_i8x16_s
    i32x4.extract_lane 0)
  (func (export "i16x8_extend_high_i8x16_s") (result i32)
    v128.const i32x4 0 0 0x01010101 0x02020202
    i16x8.extend_high_i8x16_s
    i32x4.extract_lane 0)
  (func (export "i16x8_extend_low_i8x16_u") (result i32)
    v128.const i32x4 -1 0 0 0
    i16x8.extend_low_i8x16_u
    i32x4.extract_lane 0)
  (func (export "i32x4_extend_low_i16x8_u") (result i32)
    v128.const i32x4 -1 0 0 0
    i32x4.extend_low_i16x8_u
    i32x4.extract_lane 0)
  (func (export "i32x4_extend_high_i16x8_u") (result i32)
    v128.const i32x4 0 0 0x00020002 0x00020002
    i32x4.extend_high_i16x8_u
    i32x4.extract_lane 0)
  (func (export "i64x2_extend_low_i32x4_u") (result i32)
    v128.const i32x4 7 0 0 0
    i64x2.extend_low_i32x4_u
    i32x4.extract_lane 0)
  (func (export "i16x8_extmul_low_i8x16_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.extmul_low_i8x16_s
    i32x4.extract_lane 0)
  (func (export "i16x8_extmul_high_i8x16_s") (result i32)
    v128.const i32x4 0 0 0x04040404 0x04040404
    v128.const i32x4 0 0 0x05050505 0x05050505
    i16x8.extmul_high_i8x16_s
    i32x4.extract_lane 0)
  (func (export "i32x4_extmul_low_i16x8_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.extmul_low_i16x8_s
    i32x4.extract_lane 0)
  (func (export "i16x8_extadd_pairwise_i8x16_s") (param i32) (result i32)
    local.get 0 i32x4.splat
    i16x8.extadd_pairwise_i8x16_s
    i32x4.extract_lane 0)
  (func (export "i32x4_extadd_pairwise_i16x8_s") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32x4.extadd_pairwise_i16x8_s
    i32x4.extract_lane 0)
  (func (export "i8x16_narrow_i16x8_s") (result i32)
    v128.const i32x4 0x00020002 0 0 0
    v128.const i32x4 0x00030003 0 0 0
    i8x16.narrow_i16x8_s
    i32x4.extract_lane 2)
  (func (export "i8x16_narrow_i16x8_u") (result i32)
    v128.const i32x4 -1 0 0 0
    v128.const i32x4 0 0 0 0
    i8x16.narrow_i16x8_u
    i32x4.extract_lane 0)
  (func (export "i32x4_dot_i16x8_s") (result i32)
    v128.const i32x4 0x00030002 0 0 0
    v128.const i32x4 0x00050004 0 0 0
    i32x4.dot_i16x8_s
    i32x4.extract_lane 0)
)
