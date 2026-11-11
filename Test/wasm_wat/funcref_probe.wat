;; Item 4, probe 3: a funcref crosses the boundary. Accepted or refused, never silent
;; (docs/API_ABI_STABILITY.md).
(module
  (func $identity (param i32) (result i32)
    local.get 0)
  ;; `ref.func` needs the function declared, or validation rejects the fixture.
  (elem declare func $identity)
  (func (export "keep") (param funcref) (result funcref)
    local.get 0)
  (func (export "ref") (result funcref)
    ref.func $identity))
