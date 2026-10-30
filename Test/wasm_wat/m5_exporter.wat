;; M5 exporter: defines and exports a function whose type is (i32) -> i32.
(module
  (func (export "f") (param i32) (result i32)
    local.get 0))
