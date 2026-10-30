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
  (func (export "eq") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.eq)
  (func (export "lt_s") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.lt_s)
  (func (export "lt_u") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.lt_u)
  (func (export "and") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.and)
  (func (export "or") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.or)
  (func (export "xor") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.xor)
  (func (export "shl") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.shl)
  (func (export "shr_s") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.shr_s)
  (func (export "shr_u") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.shr_u)
  (func (export "rotl") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.rotl)
  (func (export "rotr") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.rotr)
  (func (export "local_roundtrip") (param i32) (result i32) (local i32)
    local.get 0
    local.set 1
    local.get 1
    i32.const 3
    i32.mul)
)
