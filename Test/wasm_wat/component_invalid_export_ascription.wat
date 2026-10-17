;; an export's ascribed type must match the exported function type
(component
  (type $t (func))
  (import "f" (func $f (type $t)))
  (export "f" (func $f) (func (param "x" u32)))
)