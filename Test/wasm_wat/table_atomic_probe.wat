;; Shared table atomics: a shared anyref table, set/get with an ordering. The
;; final get observes null, so the result is 1.
(module
  (table 2 2 (shared anyref))

  (func (export "run") (param i32 (ref null (shared any))) (result i32)
    local.get 0
    local.get 1
    table.atomic.set seq_cst 0
    local.get 0
    table.atomic.get seq_cst 0
    ref.is_null))
