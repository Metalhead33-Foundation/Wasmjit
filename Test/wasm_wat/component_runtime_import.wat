;; A component function import is linked to a host ComponentFunction.
(component
  (import "host-add" (func $f (param "a" u32) (param "b" u32) (result u32)))
  (export "host-add" (func $f))
)
