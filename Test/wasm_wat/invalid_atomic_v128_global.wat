;; Invalid upstream: global.atomic only allows i32/i64/anyref, not v128.
(module
  (global $g (mut v128) (v128.const i32x4 0 0 0 0))
  (func (export "s") (param v128)
    local.get 0
    global.atomic.set seq_cst $g))
