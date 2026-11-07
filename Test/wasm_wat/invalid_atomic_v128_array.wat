;; Invalid upstream: array.atomic only allows i8/i16/i32/i64/anyref, not v128.
(module
  (type $a (array (mut v128)))
  (func (export "s") (param (ref null $a) i32 v128)
    local.get 0
    local.get 1
    local.get 2
    array.atomic.set seq_cst $a))
