;; Defines and exports a linear memory. Registered under module name "a", so a
;; second module can import the very same memory ("shared memory").
(module
  (memory (export "mem") 1 1)

  (func (export "store") (param i32 i32)
    local.get 0
    local.get 1
    i32.store)
  (func (export "load") (param i32) (result i32)
    local.get 0
    i32.load))
