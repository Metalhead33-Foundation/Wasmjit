;; an instantiation missing a required import argument
(component
  (component $c
    (import "x" (func))
  )
  (instance (instantiate $c))
)