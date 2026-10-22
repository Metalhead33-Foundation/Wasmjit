;; docs/proposals/MULTI_MEMORY.md: two imported memories keep distinct
;; address spaces, and memory.copy can copy between them.
(module
  (import "a" "mem0" (memory 1 1))
  (import "a" "mem1" (memory 1 1))
  (func (export "write0") (param i32 i32)
    local.get 0
    local.get 1
    i32.store 0)
  (func (export "write1") (param i32 i32)
    local.get 0
    local.get 1
    i32.store 1)
  (func (export "read0") (param i32) (result i32)
    local.get 0
    i32.load 0)
  (func (export "read1") (param i32) (result i32)
    local.get 0
    i32.load 1)
  (func (export "copy1to0") (param i32 i32 i32)
    local.get 0
    local.get 1
    local.get 2
    memory.copy 0 1))
