;; Item 4, probe 21a: a module that exports a tag AND throws it
;; (docs/TESTING.md section 15).
(module
  (tag $e (param i32))
  (export "e" (tag $e))
  (func (export "thrown") (result i32)
    i32.const 7
    throw $e))
