;; Wide AND multi-result: three vectors in, an i32 and an i64 out. The shape that
;; reaches SLJIT's frame form with two results
;; (docs/ABI.md).
(module
  (func (export "wide_pair") (param v128 v128 v128) (result i32 i64)
    local.get 0
    i32x4.extract_lane 0
    local.get 1
    i32x4.extract_lane 0
    i32.add
    local.get 2
    i32x4.extract_lane 0
    i32.add
    i64.const 9))
