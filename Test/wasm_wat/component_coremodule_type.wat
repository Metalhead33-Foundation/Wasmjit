;; A component instantiation checks the exact core module type, not just the
;; module sort.
(component
  (core type $mt (module
    (export "f" (func (param i32)))
  ))
  (import "m0" (core module $actual (type $mt)))
  (component $c
    (import "m" (core module $mm (type $mt)))
  )
  (instance $i (instantiate $c (with "m" (core module $actual))))
)
