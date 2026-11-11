;; Item 4, probe 20b: a module that IMPORTS a tag -- the cross-instance rule
;; (docs/TESTING.md section 13).
(module
  (import "provider" "e" (tag $e (param i32)))
  (func (export "payload") (result i32)
    i32.const 1))
