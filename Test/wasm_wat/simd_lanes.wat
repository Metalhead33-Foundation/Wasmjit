;; The second shared v128 slice: integer lane arithmetic, the comparison
;; families and all_true. Results are observed through i32x4 lanes, so the
;; module still needs no v128 signature.
(module
  (func (export "i8x16_add") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32.const 0x01010101 i32x4.splat
    i8x16.add
    i32x4.extract_lane 0)
  (func (export "i8x16_sub") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32.const 0x01010101 i32x4.splat
    i8x16.sub
    i32x4.extract_lane 0)
  (func (export "i16x8_mul") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.mul
    i32x4.extract_lane 0)
  (func (export "i32x4_add") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.add
    i32x4.extract_lane 2)
  (func (export "i32x4_mul") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.mul
    i32x4.extract_lane 0)
  (func (export "i64x2_add") (result i32)
    v128.const i64x2 5 0
    v128.const i64x2 7 0
    i64x2.add
    i32x4.extract_lane 0)
  (func (export "i64x2_sub") (result i32)
    v128.const i64x2 10 0
    v128.const i64x2 3 0
    i64x2.sub
    i32x4.extract_lane 0)
  (func (export "i64x2_mul") (result i32)
    v128.const i64x2 6 0
    v128.const i64x2 7 0
    i64x2.mul
    i32x4.extract_lane 0)
  (func (export "i8x16_lt_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.lt_s
    i32x4.extract_lane 0)
  (func (export "i8x16_lt_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.lt_u
    i32x4.extract_lane 0)
  (func (export "i16x8_ge_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.ge_s
    i32x4.extract_lane 0)
  (func (export "i32x4_ne") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.ne
    i32x4.extract_lane 0)
  (func (export "i32x4_gt_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.gt_u
    i32x4.extract_lane 0)
  (func (export "i64x2_lt_s") (result i32)
    v128.const i64x2 -1 0
    v128.const i64x2 0 0
    i64x2.lt_s
    i32x4.extract_lane 0)
  (func (export "i64x2_eq") (result i32)
    v128.const i64x2 9 9
    v128.const i64x2 9 9
    i64x2.eq
    i32x4.extract_lane 2)
  (func (export "i8x16_neg") (param i32) (result i32)
    local.get 0 i32x4.splat
    i8x16.neg
    i32x4.extract_lane 0)
  (func (export "i32x4_neg") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32x4.neg
    i32x4.extract_lane 1)
  (func (export "i8x16_all_true") (param i32) (result i32)
    local.get 0 i32x4.splat
    i8x16.all_true)
  (func (export "i32x4_all_true") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32x4.all_true)
)
