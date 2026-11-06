;; The runtime type test follows the subtype relation, not identity: `$t` is
;; declared `(sub $s ...)`, so an instance of `$t` must satisfy `ref.test (ref $s)`.
(module
  (type $s (sub (struct (field i32))))
  (type $t (sub $s (struct (field i32) (field i32))))
  (func (export "test_super") (result i32)
    (ref.test (ref $s) (struct.new $t (i32.const 1) (i32.const 2))))
  (func (export "test_sub") (result i32)
    (ref.test (ref $t) (struct.new $s (i32.const 1))))
  (func (export "test_null") (result i32)
    (ref.test (ref $s) (ref.null $s)))
  (func (export "test_nullable") (result i32)
    (ref.test (ref null $s) (struct.new $t (i32.const 1) (i32.const 2))))
  (func (export "cast_super") (result i32)
    (ref.is_null (ref.cast (ref $s) (struct.new $t (i32.const 1) (i32.const 2)))))
  (func (export "cast_fail") (result i32)
    (ref.is_null (ref.cast (ref $t) (struct.new $s (i32.const 1))))))
