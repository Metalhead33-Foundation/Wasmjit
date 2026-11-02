;; CM17 regression: an inline bag at component-instance index 0 and an
;; instantiated nested component at index 1. The old runtime stored nested
;; runtimes in instantiate order, so aliasing the bag's function after the
;; nested instance resolved the wrong runtime. The alias below must still
;; produce the bag's function.
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
  (instance $bag (export "f" (func $lifted)))
  (component $child
    (type $childFt (func (param "x" u32) (result u32)))
    (import "x" (func $x (type $childFt)))
    (export "run" (func $x))
  )
  (instance $nested (instantiate $child (with "x" (func $lifted))))
  (alias export $bag "f" (func $aliased))
  (export "f" (func $aliased))
)
