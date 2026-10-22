;; docs/proposals/MUTABLE_GLOBALS.md MG-2/MG-3: B imports A's mutable global.
;; A write through either instance must be visible through the other.
(module
  (import "a" "g" (global $g (mut i32)))
  (func (export "get") (result i32) (global.get $g))
  (func (export "set") (param i32) (global.set $g (local.get 0)))
)
