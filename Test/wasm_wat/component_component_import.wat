;; CM-20: a component-sort import is a post-1.0 feature and must be gated.
(component
  (type $ct (component (export "answer" (func (result u32)))))
  (import "c" (component $c (type $ct)))
  (instance $i (instantiate $c))
  (alias export $i "answer" (func $f))
  (export "answer" (func $f))
)
