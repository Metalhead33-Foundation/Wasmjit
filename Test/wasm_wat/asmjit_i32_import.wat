(module
  (import "host" "double" (func $double (param i32) (result i32)))
  (func (export "call_host") (param i32) (result i32)
    local.get 0
    call $double)
)
