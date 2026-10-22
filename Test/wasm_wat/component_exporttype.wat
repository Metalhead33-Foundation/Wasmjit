;; A top-level export with an optional ascribed type.
(component
  (type $ft (func))
  (import "f" (func $f (type $ft)))
  (export "f" (func $f) (func (type $ft)))
)