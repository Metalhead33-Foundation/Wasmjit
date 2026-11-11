;; Three vector parameters: four C argument slots short of what SLJIT can express
;; (a v128 takes two slots and the target has six saved registers), and a shape
;; LibJIT compiles fine. Kept out of v128_boundary_order.wat so that the shared
;; fixture never carries a shape one backend cannot compile
;; (docs/backends/HISTORY.md).
(module
  (func (export "three_lanes") (param v128 v128 v128) (result i32)
    local.get 0
    i32x4.extract_lane 0
    local.get 1
    i32x4.extract_lane 0
    i32.add
    local.get 2
    i32x4.extract_lane 0
    i32.add))
