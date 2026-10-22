;; an alias naming an export that the instance does not have
(component
  (import "i" (instance $i))
  (alias export $i "missing" (func))
)