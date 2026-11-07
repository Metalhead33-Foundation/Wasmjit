;; Invalid upstream: struct.atomic only allows i8/i16/i32/i64/anyref, not v128.
(module
  (type $s (shared (struct (field (mut v128)))))
  (func (export "s") (param (ref null $s) v128)
    local.get 0
    local.get 1
    struct.atomic.set seq_cst $s 0))
