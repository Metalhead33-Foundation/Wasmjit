;; Item 4, probe 7, strengthened: atomic operations that OBSERVE their result.
;; The original `atomics_probe` only proved the module instantiated; these exports
;; return the memory state the operations produced, so a lowering that ignores the
;; operation (or truncates it) is visible (docs/backends/CAPABILITY_PROBES.md
;; section 1).
(module
  (memory 1 1 shared)

  ;; i32: add, xor, compare-exchange, then load. 0 -> 5 -> 6 -> 9.
  (func (export "i32_probe") (result i32)
    i32.const 0
    i32.const 5
    i32.atomic.rmw.add
    drop
    i32.const 0
    i32.const 3
    i32.atomic.rmw.xor
    drop
    i32.const 0
    i32.const 6
    i32.const 9
    i32.atomic.rmw.cmpxchg
    drop
    i32.const 0
    i32.atomic.load)

  ;; i64: store 7, add 3, load 10.
  (func (export "i64_probe") (result i64)
    i32.const 0
    i64.const 7
    i64.atomic.store
    i32.const 0
    i64.const 3
    i64.atomic.rmw.add
    drop
    i32.const 0
    i64.atomic.load)

  ;; Narrow widths wrap: 255 + 1 -> 0 in one byte, 65535 + 1 -> 0 in two.
  (func (export "i32_8bit") (result i32)
    i32.const 4
    i32.const 255
    i32.atomic.store8
    i32.const 4
    i32.const 1
    i32.atomic.rmw8.add_u
    drop
    i32.const 4
    i32.atomic.load8_u)

  (func (export "i32_16bit") (result i32)
    i32.const 8
    i32.const 65535
    i32.atomic.store16
    i32.const 8
    i32.const 1
    i32.atomic.rmw16.add_u
    drop
    i32.const 8
    i32.atomic.load16_u))
