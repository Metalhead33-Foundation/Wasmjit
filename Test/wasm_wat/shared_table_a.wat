;; Exports a funcref table; another module (shared_table_b) imports it.
;; Together they exercise Store-owned, shared table storage and imported tables.
(module
  (table (export "table") 2 8 funcref)

  (func $f (result i32) (i32.const 123))
  (elem declare func $f)

  (func (export "size") (result i32) (table.size 0))

  (func (export "grow") (param i32) (result i32)
    (table.grow 0 (ref.null func) (local.get 0)))

  (func (export "set") (param i32)
    (table.set 0 (local.get 0) (ref.func $f)))
)
