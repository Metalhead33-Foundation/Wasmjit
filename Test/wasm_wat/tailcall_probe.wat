;; Item 4, probe 4: a tail call. Accepted or refused, never silent
;; (docs/API_ABI_STABILITY.md).
(module
  (func $callee (param i32) (result i32)
    local.get 0)
  (func (export "tail") (param i32) (result i32)
    local.get 0
    return_call $callee))
