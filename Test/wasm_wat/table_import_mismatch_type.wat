;; Imports the funcref table exported by shared_table_a as an *externref* table.
;; The element types are incompatible, so instantiation must fail at link time
;; instead of silently aliasing the storage (docs/proposals/GC.md R1).
(module
  (import "a" "table" (table 2 8 externref))
)
