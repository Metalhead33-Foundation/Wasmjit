;; Component alias export and an instantiate argument.
(component
  (import "f0" (func $f0))
  (component $c
    (import "f" (func $f))
    (export "f" (func $f))
  )
  (instance $i (instantiate $c (with "f" (func $f0))))
  (alias export $i "f" (func $f1))
)