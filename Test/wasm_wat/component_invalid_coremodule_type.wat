;; The provided module has a different function signature from the declared
;; core module type, so instantiation must reject it.
(component
  (core type $mt (module
    (export "f" (func (param i32)))
  ))
  (core type $mt2 (module
    (export "f" (func (param i64)))
  ))
  (import "m0" (core module $actual (type $mt2)))
  (component $c
    (import "m" (core module $mm (type $mt)))
  )
  (instance $i (instantiate $c (with "m" (core module $actual))))
)
