;; A memory with a declared maximum, a grow export and a byte read: the
;; `[mem-provider]` fixture for MP-1 (docs/proposals/MEMORY_PROVIDER.md §8.2).
;;
;; The host chooses how the bytes are obtained -- a growable buffer, or a
;; reservation committed on growth -- and the module cannot tell which it got,
;; because a guest addresses offsets rather than host addresses. `load` lets a
;; test read a byte of a page the host has just grown into.
(module
  (memory (export "mem") 1 4)

  (func (export "grow") (param i32) (result i32)
    local.get 0
    memory.grow)

  (func (export "size") (result i32)
    memory.size)

  (func (export "load") (param i32) (result i32)
    local.get 0
    i32.load8_u))
