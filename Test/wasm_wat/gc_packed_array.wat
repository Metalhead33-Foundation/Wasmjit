;; Packed array elements: a byte element keeps one byte, and the two reads differ
;; only in sign extension.
(module
  (type $p (array (mut i8)))
  (func (export "array_packed_get_s") (result i32)
    (array.get_s $p (array.new $p (i32.const -1) (i32.const 1)) (i32.const 0)))
  (func (export "array_packed_get_u") (result i32)
    (array.get_u $p (array.new $p (i32.const -1) (i32.const 1)) (i32.const 0)))
  (func (export "array_packed_set") (result i32) (local $x (ref $p))
    (local.set $x (array.new $p (i32.const 0) (i32.const 1)))
    (array.set $p (local.get $x) (i32.const 0) (i32.const -2))
    (array.get_s $p (local.get $x) (i32.const 0)))
)
