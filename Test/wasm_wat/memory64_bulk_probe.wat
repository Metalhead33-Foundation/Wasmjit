;; Item 4: bulk memory on a memory64 memory. The addresses and lengths are
;; 64-bit; `memory.init` keeps a 32-bit segment source/length (the data segment
;; is a 32-bit index space), which the validator enforces.
(module
  (memory i64 1)
  (data (i64.const 0) "abcdefg")
  (data "abcdefg") ;; passive: only a passive segment survives instantiation

  (func (export "copy") (param i64 i64 i64) (result i32)
    local.get 0
    local.get 1
    local.get 2
    memory.copy
    i32.const 0)

  (func (export "fill") (param i64 i32 i64) (result i32)
    local.get 0
    local.get 1
    local.get 2
    memory.fill
    i32.const 0)

  (func (export "init") (param i64 i32 i32) (result i32)
    local.get 0
    local.get 1
    local.get 2
    memory.init 1
    i32.const 0)

  (func (export "get") (param i64) (result i32)
    local.get 0
    i32.load8_u))
