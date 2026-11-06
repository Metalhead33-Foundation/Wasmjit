;; GC array slice 1: allocation through the type-derived entry point, observed
;; through `array.len` (element access needs the bounds trap and comes next).
(module
  (type $a (array (mut i32)))
  (func (export "array_len") (result i32)
    (array.len (array.new $a (i32.const 7) (i32.const 3))))
  (func (export "array_len_zero") (result i32)
    (array.len (array.new $a (i32.const 7) (i32.const 0))))
  (func (export "array_get") (result i32)
    ;; `array.new` writes element 0; later elements start zeroed.
    (array.get $a (array.new $a (i32.const 42) (i32.const 2)) (i32.const 0)))
  (func (export "array_get_oob") (result i32)
    (array.get $a (array.new $a (i32.const 42) (i32.const 2)) (i32.const 2)))
  (func (export "array_set_get") (result i32) (local $x (ref $a))
    (local.set $x (array.new $a (i32.const 1) (i32.const 2)))
    (array.set $a (local.get $x) (i32.const 1) (i32.const 77))
    (array.get $a (local.get $x) (i32.const 1)))
)
