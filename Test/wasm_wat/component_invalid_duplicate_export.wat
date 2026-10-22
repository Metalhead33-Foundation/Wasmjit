;; duplicate export names are rejected
(component
  (type $t (func))
  (import "f" (func $f (type $t)))
  (export "x" (func $f))
  (export "x" (func $f))
)