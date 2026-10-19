;; Instantiating a component with an abstract `sub resource` type substitutes
;; the provided concrete resource in the resulting instance type.
(component
  (type $r1 (resource (rep i32)))
  (type $r2 (resource (rep i32)))
  (component $c
    (import "r" (type $r (sub resource)))
    (export "r" (type $r))
  )
  (instance $i1 (instantiate $c (with "r" (type $r1))))
  (instance $i2 (instantiate $c (with "r" (type $r2))))
  (instance $i3 (instantiate $c (with "r" (type $r1))))
)
