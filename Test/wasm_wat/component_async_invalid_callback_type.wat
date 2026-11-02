;; CM16: a callback must have type (param i32 i32 i32) (result i32).
(component
  (core module $M
    (func (export "run") (param i32) (result i32) (i32.const 0))
    (func (export "cb") (param i32) (result i32) (local.get 0)))
  (core instance $ci (instantiate $M))
  (alias core export $ci "cb" (core func $cb))
  (type $ft (func async (param "x" u32) (result u32)))
  (canon lift (core func $ci "run") async (callback $cb)
    (func $run (type $ft)))
  (export "run" (func $run))
)
