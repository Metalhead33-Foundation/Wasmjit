;; A component type with an abstract resource import is an open scheme and must
;; not receive a closed Store TypeId.
(component
  (type $open (component
    (import "r" (type (sub resource)))
    (export "f" (func))
  ))
)
