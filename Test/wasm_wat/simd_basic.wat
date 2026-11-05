;; The first shared v128 slice: value transport, i32x4 lane access, bitwise
;; operations and any_true. Every family is exercised through an i32 result so
;; the module does not need a v128 signature (not lowered yet).
(module
  (memory 1)
  (func (export "const_extract") (result i32)
    v128.const i32x4 1 2 3 4
    i32x4.extract_lane 2)
  (func (export "const_lane1") (result i32)
    v128.const i32x4 1 2 3 4
    i32x4.extract_lane 1)
  (func (export "const_lane3") (result i32)
    v128.const i32x4 1 2 3 4
    i32x4.extract_lane 3)
  (func (export "splat_extract") (param i32) (result i32)
    local.get 0
    i32x4.splat
    i32x4.extract_lane 3)
  (func (export "store_load") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32x4.splat
    v128.store
    local.get 0
    v128.load
    i32x4.extract_lane 1)
  (func (export "replace_extract") (param i32) (result i32)
    v128.const i32x4 1 2 3 4
    local.get 0
    i32x4.replace_lane 1
    i32x4.extract_lane 1)
  (func (export "and_value") (result i32)
    v128.const i32x4 0x0F0F0F0F 0 0 0
    v128.const i32x4 0x33333333 0 0 0
    v128.and
    i32x4.extract_lane 0)
  (func (export "or_value") (result i32)
    v128.const i32x4 0x0F0F0F0F 0 0 0
    v128.const i32x4 0x33333333 0 0 0
    v128.or
    i32x4.extract_lane 0)
  (func (export "xor_value") (result i32)
    v128.const i32x4 0x0F0F0F0F 0 0 0
    v128.const i32x4 0x33333333 0 0 0
    v128.xor
    i32x4.extract_lane 0)
  (func (export "andnot_value") (result i32)
    v128.const i32x4 0xFFFFFFFF 0 0 0
    v128.const i32x4 0x0F0F0F0F 0 0 0
    v128.andnot
    i32x4.extract_lane 0)
  (func (export "not_value") (result i32)
    v128.const i32x4 0x0F0F0F0F 0 0 0
    v128.not
    i32x4.extract_lane 0)
  (func (export "bitselect_value") (result i32)
    v128.const i32x4 0xAAAAAAAA 0 0 0
    v128.const i32x4 0x55555555 0 0 0
    v128.const i32x4 0xFFFF0000 0 0 0
    v128.bitselect
    i32x4.extract_lane 0)
  (func (export "any_true") (param i32) (result i32)
    local.get 0
    i32x4.splat
    v128.any_true)
  (func (export "local_roundtrip") (param i32) (result i32) (local v128)
    local.get 0
    i32x4.splat
    local.set 1
    local.get 1
    i32x4.extract_lane 0)
)
