;; Item 4, probe 5: a tail call from a FRAME-FORM function. Three vectors force the
;; frame, so the tail call has to cross a trampoline that may not carry a frame
;; pointer (docs/backends/PRODUCTION_GAPS.md section 22).
(module
  (func $callee (param v128 v128 v128) (result i32)
    local.get 0
    i32x4.extract_lane 0)
  (func (export "tail_wide") (param v128 v128 v128) (result i32)
    local.get 0
    local.get 1
    local.get 2
    return_call $callee))
