(module
  (import "provider" "double64" (func $double64 (param i64) (result i64)))
  (func (export "call_provider64") (param i64) (result i64)
    local.get 0
    call $double64)
)
