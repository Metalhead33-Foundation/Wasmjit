(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (param i32) (result i32)
    local.get 0
    i32.eqz)
)
