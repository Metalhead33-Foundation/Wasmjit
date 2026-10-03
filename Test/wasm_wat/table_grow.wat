;; Exercises a Store-owned table: table.size and table.grow (with a bounded
;; maximum so the failure path is observable).
(module
  (table 2 5 funcref)

  (func (export "size") (result i32)
    table.size 0)

  (func (export "grow") (param i32) (result i32)
    ref.null func
    local.get 0
    table.grow 0))
