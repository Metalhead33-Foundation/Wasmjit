;; Imports the same funcref table (2..8) but declares an initial size of 5.
;; A table that is already too small cannot satisfy the import
;; (docs/proposals/GC.md R1).
(module
  (import "a" "table" (table 5 8 funcref))
)
