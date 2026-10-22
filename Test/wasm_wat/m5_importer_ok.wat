;; M5 importer with a matching import type: must resolve.
(module
  (import "e" "f" (func $f (param i32) (result i32)))
  (func (export "g") (param i32) (result i32)
    local.get 0
    call $f))
