;; A 64-bit-index linear memory and table (memory64 / table64).
;;
;; The point of this module is to exercise the *width* of the plumbing:
;; memory.size / memory.grow and table.size / table.grow must produce and
;; accept 64-bit values, and a huge i64 delta must be rejected as -1 rather
;; than truncated to a plausible-looking small page count.
;; See docs/proposals/MEMORY64.md (M64-1, M4).
(module
  (memory i64 1 4)
  (table i64 1 4 funcref)

  (func (export "store") (param i64 i32)
    local.get 0
    local.get 1
    i32.store)
  (func (export "load") (param i64) (result i32)
    local.get 0
    i32.load)

  (func (export "size") (result i64)
    memory.size)
  (func (export "grow") (param i64) (result i64)
    local.get 0
    memory.grow)

  (func (export "table_size") (result i64)
    table.size)
  (func (export "table_grow") (param i64) (result i64)
    ref.null func
    local.get 0
    table.grow))
