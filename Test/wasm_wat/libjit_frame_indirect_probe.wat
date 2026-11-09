;; Frame-form call_indirect: four parameters force the staging frame.
(module
  (type $t (func (param i32 i32 i32 i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $sum)

  (func $sum (param i32 i32 i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add
    local.get 2
    i32.add
    local.get 3
    i32.add)

  (func (export "run") (result i32)
    i32.const 1
    i32.const 2
    i32.const 3
    i32.const 4
    i32.const 0
    call_indirect (type $t)))
