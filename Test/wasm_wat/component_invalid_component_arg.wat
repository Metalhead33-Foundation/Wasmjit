;; instantiation arguments must match component import names
(component
  (component $c
    (import "x" (func))
  )
  (import "g" (func $g))
  (instance (instantiate $c (with "y" (func $g))))
)