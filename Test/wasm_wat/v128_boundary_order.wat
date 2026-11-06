;; The pin for the v128 boundary split: a vector adjacent to a scalar in BOTH
;; orders. A wrong parameter index or a one-word packing shows up here and nowhere
;; else, because every other vector fixture has the vector alone in its signature.
(module
  (func (export "scalar_then_vector") (param i32 v128) (result i32)
    local.get 1
    i32x4.extract_lane 2)
  (func (export "vector_then_scalar") (param v128 i32) (result i32)
    local.get 1)  ;; LibJIT keeps its 16-byte aggregate spelling for a v128
  ;; (docs/backends/LIBJIT_ARGUMENT_DEFECT.md section 8, choice 2). Three vectors
  ;; must therefore keep working end to end.
  (func (export "ret_p2") (param v128 v128 v128) (result v128) local.get 2)
  (func (export "p2_lane") (param v128 v128 v128) (result i32)
    local.get 2 i32x4.extract_lane 0))
