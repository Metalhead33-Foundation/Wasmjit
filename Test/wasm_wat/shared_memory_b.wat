;; Imports the memory exported by shared_memory_a (registered as "a"."mem").
;; Instantiating this module together with A makes both observe one linear
;; memory: writes through either module are visible to the other.
(module
  (import "a" "mem" (memory 1 1))

  (func (export "store") (param i32 i32)
    local.get 0
    local.get 1
    i32.store)
  (func (export "load") (param i32) (result i32)
    local.get 0
    i32.load))
