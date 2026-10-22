;; memory.discard fixture (docs/proposals/MEMORY_CONTROL.md MC-1/MC-2).
;; The instruction aligns the start down and the end up to a 64 KiB page, so
;; discarding a one-byte range inside a page zeroes the whole page.
(module
  (memory $m32 (export "mem32") 2)
  (memory $m64 (export "mem64") i64 1)
  (memory $ms (export "shared") 2 2 shared)

  (func (export "store8") (param i32 i32)
    local.get 0 local.get 1 i32.store8)
  (func (export "load8") (param i32) (result i32)
    local.get 0 i32.load8_u)
  (func (export "discard") (param i32 i32)
    local.get 0 local.get 1 memory.discard)

  (func (export "store8_64") (param i64 i32)
    local.get 0 local.get 1 i32.store8 $m64)
  (func (export "load8_64") (param i64) (result i32)
    local.get 0 i32.load8_u $m64)
  (func (export "discard64") (param i64 i64)
    local.get 0 local.get 1 memory.discard $m64)

  (func (export "store8_s") (param i32 i32)
    local.get 0 local.get 1 i32.store8 $ms)
  (func (export "load8_s") (param i32) (result i32)
    local.get 0 i32.load8_u $ms)
  (func (export "discard_s") (param i32 i32)
    local.get 0 local.get 1 memory.discard $ms))
