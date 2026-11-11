;; Item 4, probe 11: bulk memory on an ordinary 32-bit memory
;; (docs/TESTING.md section 7).
(module
  (memory 1)
  (func (export "copy") (param i32 i32 i32)
    local.get 0
    local.get 1
    local.get 2
    memory.copy))
