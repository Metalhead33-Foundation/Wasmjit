;; GC construction slice 1: `struct.new` + `struct.get`, and the first fixture in
;; the project where `ref.eq` compares two non-null (eqref) objects.
(module
  (type $pair (struct (field i32)))
  (func (export "struct_roundtrip") (result i32)
    (struct.get $pair 0 (struct.new $pair (i32.const 42))))
  (func (export "struct_field_other") (result i32)
    (struct.get $pair 0 (struct.new $pair (i32.const 7))))
  (func (export "two_structs_not_equal") (result i32)
    (ref.eq (struct.new $pair (i32.const 1)) (struct.new $pair (i32.const 2))))
)
