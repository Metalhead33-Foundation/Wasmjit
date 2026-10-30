(module
  (import "provider" "double" (func $double (param i32) (result i32)))
  (func (export "call_provider") (param i32) (result i32)
    local.get 0
    call $double)
)
