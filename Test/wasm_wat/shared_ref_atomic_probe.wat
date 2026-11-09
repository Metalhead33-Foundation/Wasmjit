;; Shared reference slot: a shared struct with a `(ref null (shared eq))` field.
;; Exercises set/xchg/cmpxchg/get through the reference barrier. A null shared
;; reference is stored and exchanged; the final get observes null.
(module
  (type $box (shared (struct (field (mut (ref null (shared eq)))))))
  (type $arr (array (mut (ref null (shared eq)))))

  (func (export "run") (param (ref null (shared eq))) (result i32)
    (local $x (ref null $box))
    struct.new_default $box
    local.set $x
    local.get $x
    local.get 0
    struct.atomic.set seq_cst $box 0
    local.get $x
    local.get 0
    struct.atomic.rmw.xchg seq_cst $box 0
    drop
    local.get $x
    local.get 0
    local.get 0
    struct.atomic.rmw.cmpxchg seq_cst $box 0
    drop
    local.get $x
    struct.atomic.get seq_cst $box 0
    ref.is_null)

  (func (export "run_array") (param (ref null (shared eq))) (result i32)
    (local $a (ref null $arr))
    i32.const 1
    array.new_default $arr
    local.set $a
    local.get $a
    i32.const 0
    local.get 0
    array.atomic.set seq_cst $arr
    local.get $a
    i32.const 0
    local.get 0
    array.atomic.rmw.xchg seq_cst $arr
    drop
    local.get $a
    i32.const 0
    local.get 0
    local.get 0
    array.atomic.rmw.cmpxchg seq_cst $arr
    drop
    local.get $a
    i32.const 0
    array.atomic.get seq_cst $arr
    ref.is_null))
