;; Multi-table: two independent funcref tables in one module.
;; Exercises non-zero table indices for size/grow/fill/copy/init and
;; call_indirect, and checks that the two tables do not alias each other.
(module
  (type $ret_i32 (func (result i32)))

  (table $t0 (export "t0") 4 8 funcref)
  (table $t1 (export "t1") 2 8 funcref)

  (func $fortyTwo (result i32) (i32.const 42))
  (func $seven (result i32) (i32.const 7))
  (func $nine (result i32) (i32.const 9))

  ;; t0[0] = $fortyTwo, t0[1] = $seven
  (elem (table $t0) (i32.const 0) func $fortyTwo $seven)
  ;; passive segment, used by table.init into t1
  (elem $passive func $nine $fortyTwo)
  ;; t1[0] = $seven
  (elem (table $t1) (i32.const 0) func $seven)

  (func (export "size0") (result i32) (table.size $t0))
  (func (export "size1") (result i32) (table.size $t1))

  (func (export "grow1") (param i32) (result i32)
    (table.grow $t1 (ref.null func) (local.get 0)))

  (func (export "call0") (param i32) (result i32)
    (call_indirect $t0 (type $ret_i32) (local.get 0)))
  (func (export "call1") (param i32) (result i32)
    (call_indirect $t1 (type $ret_i32) (local.get 0)))

  (func (export "init1") (param i32 i32 i32)
    (table.init $t1 $passive (local.get 0) (local.get 1) (local.get 2)))

  ;; t1[dst..] = t0[src..]
  (func (export "copy10") (param i32 i32 i32)
    (table.copy $t1 $t0 (local.get 0) (local.get 1) (local.get 2)))

  (func (export "fill1") (param i32 i32)
    (table.fill $t1 (local.get 0) (ref.func $nine) (local.get 1)))
)
