;; Item 4, probe 18: atomics beyond a single RMW -- store then load
;; (docs/TESTING.md section 11).
(module
  (memory 1 1 shared)
  (func (export "store_load") (result i32)
    i32.const 0
    i32.const 123
    i32.atomic.store
    i32.const 0
    i32.atomic.load))
