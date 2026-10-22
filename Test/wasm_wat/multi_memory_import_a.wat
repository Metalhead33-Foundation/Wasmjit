;; docs/proposals/MULTI_MEMORY.md: an exporter with two memories, used by the
;; imported-multi-memory link test.
(module
  (memory (export "mem0") 1 1)
  (memory (export "mem1") 1 1)
  (func (export "read0") (param i32) (result i32)
    local.get 0
    i32.load 0)
  (func (export "read1") (param i32) (result i32)
    local.get 0
    i32.load 1))
