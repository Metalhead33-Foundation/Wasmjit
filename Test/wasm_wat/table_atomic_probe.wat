;; Shared table atomics: an anyref table exercises set/get/xchg, and an eqref
;; table exercises cmpxchg (the proposal restricts cmpxchg to eqref). A null
;; shared reference is stored; the final get observes null, so the result is 1.
(module
  (table 2 2 (shared anyref))
  (table 2 2 (shared eqref))

  (func (export "run") (param i32 (ref null (shared eq))) (result i32)
    local.get 0
    local.get 1
    table.atomic.set seq_cst 0
    local.get 0
    local.get 1
    table.atomic.rmw.xchg seq_cst 0
    drop
    local.get 0
    local.get 1
    local.get 1
    table.atomic.rmw.cmpxchg seq_cst 1
    drop
    local.get 0
    table.atomic.get seq_cst 0
    ref.is_null))
