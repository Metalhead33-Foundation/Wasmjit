;; Shared table atomics. `set`/`get` round-trip a non-null reference, and the
;; read-modify-write forms return the element they displaced, so the fixture
;; compares that old value with `ref.eq` instead of only dropping it: a lowering
;; that treated `xchg`/`cmpxchg` as a plain store would answer 0 here. The
;; proposal restricts `cmpxchg` to eqref, so the anyref table covers set/get and
;; a displaced-value `xchg`, while an eqref table covers xchg/cmpxchg on both
;; the success and the failure path.
(module
  (type $box (shared (struct (field (mut i32)))))
  (table 2 2 (shared anyref))
  (table 2 2 (shared eqref))

  (func (export "run") (result i32)
    (local $any (ref null (shared any)))
    (local $a (ref null (shared eq)))
    (local $b (ref null (shared eq)))
    (local $ok i32)

    ;; The anyref table stores a shared struct, and get observes it.
    struct.new_default $box
    local.set $any
    i32.const 0
    local.get $any
    table.atomic.set seq_cst 0
    i32.const 0
    table.atomic.get seq_cst 0
    ref.is_null
    i32.eqz
    local.set $ok

    ;; xchg on the anyref table returns the element it displaced: the table
    ;; held $any, so exchanging in null answers non-null.
    i32.const 0
    ref.null (shared any)
    table.atomic.rmw.xchg seq_cst 0
    ref.is_null
    i32.eqz
    local.get $ok
    i32.and
    local.set $ok

    ;; Two distinct shared structs make the eqref results comparable.
    struct.new_default $box
    local.set $a
    struct.new_default $box
    local.set $b
    i32.const 0
    local.get $a
    table.atomic.set seq_cst 1

    ;; xchg returns the displaced $a and leaves $b in the slot.
    i32.const 0
    local.get $b
    table.atomic.rmw.xchg seq_cst 1
    local.get $a
    ref.eq
    local.get $ok
    i32.and
    local.set $ok

    ;; A successful cmpxchg returns the value it replaced ($b) ...
    i32.const 0
    local.get $b
    local.get $a
    table.atomic.rmw.cmpxchg seq_cst 1
    local.get $b
    ref.eq
    local.get $ok
    i32.and
    local.set $ok

    ;; ... and a failed one returns the current value ($a) without swapping.
    i32.const 0
    ref.null (shared eq)
    local.get $b
    table.atomic.rmw.cmpxchg seq_cst 1
    local.get $a
    ref.eq
    local.get $ok
    i32.and
    local.set $ok

    local.get $ok))
