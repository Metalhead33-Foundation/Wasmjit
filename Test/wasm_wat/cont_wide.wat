;; Wide continuation payloads: two i32 parameters and two i32 results, plus an
;; externref identity payload. The target is invoked through the backend's
;; `jit_apply` thunk, not through a scalar C signature.
(module
  (type $ft (func (param i32 i32) (result i32 i32)))
  (type $ct (cont $ft))
  (type $fr (func (param externref) (result externref)))
  (type $cr (cont $fr))

  (func $gen (type $ft)
    local.get 0
    local.get 1
    i32.add
    local.get 0
    local.get 1
    i32.sub)

  (func $id (type $fr)
    local.get 0)

  (elem declare func $gen $id)

  (func (export "multi") (result i32)
    i32.const 10
    i32.const 4
    ref.func $gen
    cont.new $ct
    resume $ct
    i32.add)

  (func (export "ref") (param externref) (result externref)
    local.get 0
    ref.func $id
    cont.new $cr
    resume $cr))
