;; Shared reference slot: a shared struct with a `(ref null (shared any))` field.
;; Exercises set/xchg/get through the reference barrier. A null shared reference is
;; stored and exchanged; the result is 1 when the final get observes null.
(module
  (type $box (shared (struct (field (mut (ref null (shared any)))))))

  (func (export "run") (param (ref null (shared any))) (result i32)
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
    struct.atomic.get seq_cst $box 0
    ref.is_null))

