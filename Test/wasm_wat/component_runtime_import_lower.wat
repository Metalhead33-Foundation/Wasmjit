;; Lower a host component function import to a core callable.
(component
  (import "host-add" (func $f (param "a" u32) (param "b" u32) (result u32)))
  (canon lower (func $f) (core func $cf))
)
