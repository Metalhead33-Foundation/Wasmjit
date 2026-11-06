;; One export per trap kind the GC surface produces, so the *kind* is asserted and
;; not merely the fact that something trapped.
(module
  (type $p (struct (field i32)))
  (type $q (struct (field i64)))
  (type $a (array (mut i32)))
  (func (export "struct_null") (result i32)
    (struct.get $p 0 (ref.null $p)))
  (func (export "array_null") (result i32)
    (array.len (ref.null $a)))
  (func (export "array_oob") (result i32)
    (array.get $a (array.new $a (i32.const 1) (i32.const 1)) (i32.const 1)))
  (func (export "cast_fail") (result i32)
    (ref.is_null (ref.cast (ref $q) (struct.new $p (i32.const 1)))))
)
