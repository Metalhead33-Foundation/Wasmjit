;; Item 4, probe 14: array operations beyond construction -- array.set and array.fill.
;; Two earlier versions of this fixture were INVALID wasm (a missing fill count, then a
;; get after the array had been consumed), and the backends' "operand stack underflow"
;; messages were correct rejections of the probe, not of the feature
;; (docs/backends/CAPABILITY_PROBES.md section 8).
(module
  (type $row (array (mut i32)))
  (func (export "probe") (result i32)
    ;; array.set: index 0 becomes 9
    i32.const 5
    i32.const 2
    array.new $row
    i32.const 0
    i32.const 9
    array.set $row
    ;; array.fill: (array, offset, value, count)
    i32.const 5
    i32.const 2
    array.new $row
    i32.const 0
    i32.const 5
    i32.const 2
    array.fill $row
    i32.const 0))
