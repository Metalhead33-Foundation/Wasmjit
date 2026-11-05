;; `select` and `select_t` on v128: the untyped form needs the 16-byte slot
;; select, the typed form additionally proves the type check accepts vectors.
(module
  (func (export "select_v128") (param i32) (result i32)
    v128.const i32x4 1 1 1 1
    v128.const i32x4 2 2 2 2
    local.get 0
    select (result v128)
    i32x4.extract_lane 2)
  (func (export "select_v128_two_words") (param i32) (result i32)
    v128.const i32x4 7 8 9 10
    v128.const i32x4 3 4 5 6
    local.get 0
    select (result v128)
    i32x4.extract_lane 3)
  (func (export "select_v128_untyped") (param i32) (result i32)
    v128.const i32x4 11 12 13 14
    v128.const i32x4 1 2 3 4
    local.get 0
    select
    i32x4.extract_lane 1)
)
