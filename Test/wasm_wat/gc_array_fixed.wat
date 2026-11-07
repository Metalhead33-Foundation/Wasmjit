;; `array.new_fixed`: the element count is an immediate, so every store offset is a
;; compile-time constant and no loop is needed.
(module
  (type $a (array (mut i32)))
  (func (export "fixed_get0") (result i32)
    (array.get $a (array.new_fixed $a 3 (i32.const 10) (i32.const 20) (i32.const 30))
      (i32.const 0)))
  (func (export "fixed_get2") (result i32)
    (array.get $a (array.new_fixed $a 3 (i32.const 10) (i32.const 20) (i32.const 30))
      (i32.const 2)))
  (func (export "fixed_len") (result i32)
    (array.len (array.new_fixed $a 3 (i32.const 1) (i32.const 2) (i32.const 3))))
)
