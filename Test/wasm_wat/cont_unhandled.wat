;; Stage-2 stack switching: a suspension with no handler. The runtime creates
;; the replacement handle, the lowering drops it and reports an internal trap;
;; the worker is cancelled and joined instead of being left suspended.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (tag $yield (param i32) (result i32))

  (func $gen (type $ft)
    i32.const 7
    suspend $yield)

  (elem declare func $gen)

  (func (export "run") (type $ft)
    ref.func $gen
    cont.new $ct
    resume $ct))
