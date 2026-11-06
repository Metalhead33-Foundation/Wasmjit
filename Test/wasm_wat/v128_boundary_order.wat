;; The pin for the v128 boundary split: a vector adjacent to a scalar in BOTH
;; orders. A wrong parameter index or a one-word packing shows up here and nowhere
;; else, because every other vector fixture has the vector alone in its signature.
(module
  (func (export "scalar_then_vector") (param i32 v128) (result i32)
    local.get 1
    i32x4.extract_lane 2)
  (func (export "vector_then_scalar") (param v128 i32) (result i32)
    local.get 1))
