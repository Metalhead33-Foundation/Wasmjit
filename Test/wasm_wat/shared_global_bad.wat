;; docs/proposals/MUTABLE_GLOBALS.md MG-4: an immutable import of A's mutable
;; global must be rejected at link time.
(module
  (import "a" "g" (global $g i32))
  (func (export "get") (result i32) (global.get $g))
)
