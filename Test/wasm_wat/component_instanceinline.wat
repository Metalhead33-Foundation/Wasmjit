;; Instance expression with inline exports.
(component
  (import "f" (func $f))
  (instance $i (export "f" (func $f)))
)