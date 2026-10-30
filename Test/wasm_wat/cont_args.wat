;; Stage-2 stack switching: a continuation function with an i32 parameter. The
;; first resume supplies it; the worker calls the target with that argument and
;; the completed value comes back through resume.
(module
  (type $ft (func (param i32) (result i32)))
  (type $ct (cont $ft))

  (func $gen (type $ft)
    local.get 0
    i32.const 1
    i32.add)

  (elem declare func $gen)

  (func (export "run") (param i32) (result i32)
    local.get 0
    ref.func $gen
    cont.new $ct
    resume $ct))
