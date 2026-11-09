;; Item 4, probe 4: a tail call. Accepted or refused, never silent
;; (docs/backends/PRODUCTION_GAPS.md section 19).
(module
  (func $callee (param i32) (result i32)
    local.get 0)
  (func (export "tail") (param i32) (result i32)
    local.get 0
    return_call $callee))
