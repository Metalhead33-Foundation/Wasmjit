;; Stage-2 stack switching: `cont.new` + `resume` where the continuation
;; function completes normally. The handler list is empty, so the run function
;; simply returns the resumed function's result.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))

  (func $gen (type $ft)
    i32.const 42)

  (elem declare func $gen)

  (func (export "run") (type $ft)
    ref.func $gen
    cont.new $ct
    resume $ct))
