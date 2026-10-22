;; docs/proposals/MUTABLE_GLOBALS.md MG-5: module A owns a mutable global.
(module
  (global $g (export "g") (mut i32) (i32.const 1))
  (func (export "get") (result i32) (global.get $g))
  (func (export "set") (param i32) (global.set $g (local.get 0)))
)
