;; Item 4's first probe: a declared tag and a `throw`. Under the no-partial-modules
;; ruling the only acceptable outcomes are "instantiation throws with a reason" or
;; "the call traps" -- never a normal return
;; (docs/backends/PRODUCTION_GAPS.md section 16).
(module
  (tag $e (param i32))
  (func (export "do_throw") (param i32) (result i32)
    local.get 0
    throw $e))
