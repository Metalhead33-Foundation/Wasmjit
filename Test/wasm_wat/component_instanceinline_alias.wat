;; A component-level inline export bag, consumed through an export alias. The
;; runtime used to throw "inline component instances are not supported" on the
;; instance definition, so this fixture is CM12's consumer test. No host import
;; is needed: the bag re-exports a lifted component function.
(component
  (core module $m
    (func (export "f") (param i32) (result i32)
      (local.get 0)
    )
  )
  (core instance $ci (instantiate $m))
  (alias core export $ci "f" (core func $cf))
  (type $ft (func (param "x" u32) (result u32)))
  (canon lift (core func $cf) (func $lifted (type $ft)))
  (instance $i (export "f" (func $lifted)))
  (alias export $i "f" (func $g))
  (export "g" (func $g))
)
