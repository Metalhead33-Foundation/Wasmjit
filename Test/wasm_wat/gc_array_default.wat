;; `array.new_default` zero-fills a runtime number of elements: the first
;; backend-emitted counted loop.
(module
  (type $a (array (mut i32)))
  (type $b (array (mut i8)))
  (func (export "default_get0") (result i32)
    (array.get $a (array.new_default $a (i32.const 4)) (i32.const 0)))
  (func (export "default_get3") (result i32)
    (array.get $a (array.new_default $a (i32.const 4)) (i32.const 3)))
  (func (export "default_len") (result i32)
    (array.len (array.new_default $a (i32.const 7))))
  (func (export "default_empty") (result i32)
    (array.len (array.new_default $a (i32.const 0))))
  (func (export "default_packed") (result i32)
    (array.get_u $b (array.new_default $b (i32.const 3)) (i32.const 2)))
)
