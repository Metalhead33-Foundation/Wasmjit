;; a core instantiation must supply every imported module
(component
  (core module $m
    (import "m" "f" (func))
  )
  (core instance (instantiate $m))
)