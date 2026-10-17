;; Imports the table exported by module "a" (see shared_table_a.wat) and
;; therefore shares its Store-owned storage: growth and writes by either
;; instance must be visible to the other.
(module
  (import "a" "table" (table 2 8 funcref))

  (func (export "size") (result i32) (table.size 0))

  (func (export "grow") (param i32) (result i32)
    (table.grow 0 (ref.null func) (local.get 0)))

  (func (export "is_null") (param i32) (result i32)
    (ref.is_null (table.get 0 (local.get 0))))
)
