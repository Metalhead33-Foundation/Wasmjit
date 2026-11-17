;; No memory at all: the Adaptive byte-order policy's closed() evidence is false
;; for a different reason (there is nothing to observe), so this fixture pins the
;; "no memory" answer rather than the interesting one.
(module
  (func (export "answer") (result i32) (i32.const 42))
)
