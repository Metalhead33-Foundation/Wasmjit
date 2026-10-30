(module
  (type $binary (func (param i32 i32) (result i32)))
  (func (export "add") (type $binary)
    local.get 0
    local.get 1
    i32.add)
  (func (export "sub") (type $binary)
    local.get 0
    local.get 1
    i32.sub)
  (func (export "mul") (type $binary)
    local.get 0
    local.get 1
    i32.mul)
  (func (export "local_roundtrip") (param i32) (result i32) (local i32)
    local.get 0
    local.set 1
    local.get 1
    i32.const 3
    i32.mul)
)
