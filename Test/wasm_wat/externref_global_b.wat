;; docs/proposals/REFERENCE_TYPES.md RT4/RT5: B imports A's mutable externref
;; global. A write through either instance must be visible through the other.
(module
  (import "a" "g" (global $g (mut externref)))
  (func (export "get") (result externref) global.get $g)
  (func (export "set") (param externref) (global.set $g (local.get 0))))
