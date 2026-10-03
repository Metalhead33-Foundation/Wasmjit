;; Two linear memories in one module (multi-memory proposal).
;; write0/read0 target memory 0, write1/read1 target memory 1.
(module
  (memory (export "mem0") 1 1)
  (memory (export "mem1") 1 1)

  (func (export "write0") (param i32 i32)
    local.get 0
    local.get 1
    i32.store 0)
  (func (export "read0") (param i32) (result i32)
    local.get 0
    i32.load 0)

  (func (export "write1") (param i32 i32)
    local.get 0
    local.get 1
    i32.store 1)
  (func (export "read1") (param i32) (result i32)
    local.get 0
    i32.load 1))
