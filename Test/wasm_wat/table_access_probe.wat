;; Table accessors: an externref table round-trips a null, a funcref table
;; round-trips a `ref.func`, and an out-of-bounds get traps.
(module
  (table 4 externref)
  (table 2 funcref)
  (elem declare func $target)

  (func $target (result i32)
    i32.const 7)

  (func (export "ext") (param i32 externref) (result i32)
    local.get 0
    local.get 1
    table.set 0
    local.get 0
    table.get 0
    ref.is_null)

  (func (export "fun") (result i32)
    i32.const 0
    ref.func $target
    table.set 1
    i32.const 0
    table.get 1
    ref.is_null)

  (func (export "oob") (result i32)
    i32.const 4
    table.get 0
    ref.is_null))
