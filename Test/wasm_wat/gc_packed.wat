;; Packed struct fields: the store keeps one byte, the two reads differ only in
;; sign extension.
(module
  (type $p (struct (field i8)))
  (func (export "packed_get_s") (result i32)
    (struct.get_s $p 0 (struct.new $p (i32.const -1))))
  (func (export "packed_get_u") (result i32)
    (struct.get_u $p 0 (struct.new $p (i32.const -1))))
  (func (export "packed_roundtrip") (result i32)
    (struct.get_s $p 0 (struct.new $p (i32.const 127))))
)
