;; `ref.cast`: the type-id check plus the null rule of the nullable form.
(module
  (type $a (struct (field i32)))
  (type $b (struct (field i64)))
  (func (export "cast_ok") (result i32)
    (ref.is_null (ref.cast (ref $a) (struct.new $a (i32.const 1)))))
  (func (export "cast_fail") (result i32)
    (ref.is_null (ref.cast (ref $b) (struct.new $a (i32.const 1)))))
  (func (export "cast_null_fail") (result i32)
    (ref.is_null (ref.cast (ref $a) (ref.null $a))))
  (func (export "cast_null_ok") (result i32)
    (ref.is_null (ref.cast (ref null $a) (ref.null $a))))
)
