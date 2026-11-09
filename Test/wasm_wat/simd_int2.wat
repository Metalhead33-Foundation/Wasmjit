;; The third shared v128 slice: shifts (the mixed-signature family), saturating
;; arithmetic, min/max, avgr, abs and popcnt. Results are observed through
;; i32x4 lanes, so the module still needs no v128 signature.
(module
  (func (export "i8x16_shl") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i8x16.shl
    i32x4.extract_lane 0)
  (func (export "i8x16_shr_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i8x16.shr_u
    i32x4.extract_lane 0)
  (func (export "i16x8_shl") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i16x8.shl
    i32x4.extract_lane 0)
  (func (export "i32x4_shr_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.shr_s
    i32x4.extract_lane 0)
  (func (export "i32x4_shr_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.shr_u
    i32x4.extract_lane 0)
  (func (export "i64x2_shr_u") (result i32)
    v128.const i64x2 -1 0
    i32.const 1
    i64x2.shr_u
    i32x4.extract_lane 1)
  (func (export "i8x16_add_sat_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.add_sat_s
    i32x4.extract_lane 0)
  (func (export "i8x16_sub_sat_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.sub_sat_u
    i32x4.extract_lane 0)
  (func (export "i16x8_add_sat_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.add_sat_u
    i32x4.extract_lane 0)
  (func (export "i16x8_q15mulr_sat_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.q15mulr_sat_s
    i32x4.extract_lane 0)
  (func (export "i8x16_min_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.min_s
    i32x4.extract_lane 0)
  (func (export "i8x16_max_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.max_u
    i32x4.extract_lane 0)
  (func (export "i32x4_min_s") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.min_s
    i32x4.extract_lane 0)
  (func (export "i32x4_max_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i32x4.max_u
    i32x4.extract_lane 0)
  (func (export "i8x16_avgr_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i8x16.avgr_u
    i32x4.extract_lane 0)
  (func (export "i16x8_avgr_u") (param i32 i32) (result i32)
    local.get 0 i32x4.splat
    local.get 1 i32x4.splat
    i16x8.avgr_u
    i32x4.extract_lane 0)
  (func (export "i8x16_abs") (param i32) (result i32)
    local.get 0 i32x4.splat
    i8x16.abs
    i32x4.extract_lane 0)
  (func (export "i32x4_abs") (param i32) (result i32)
    local.get 0 i32x4.splat
    i32x4.abs
    i32x4.extract_lane 0)
  (func (export "i8x16_popcnt") (param i32) (result i32)
    local.get 0 i32x4.splat
    i8x16.popcnt
    i32x4.extract_lane 0)
)
