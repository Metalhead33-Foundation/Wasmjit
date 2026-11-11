;; Item 4, probe 20a: a module that EXPORTS a tag
;; (docs/TESTING.md section 13).
(module
  (tag $e (param i32))
  (export "e" (tag $e))
  (func (export "payload") (result i32)
    i32.const 1))
