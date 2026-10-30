(module
  (type $unary64 (func (param i64) (result i64)))
  (table 1 funcref)
  (memory 1)
  (func $i64_double (type $unary64)
    local.get 0
    i64.const 2
    i64.mul)
  (elem (i32.const 0) $i64_double)
  (func (export "i64_call_direct") (param i64) (result i64)
    local.get 0
    call $i64_double)
  (func (export "i64_call_indirect") (param i64) (result i64)
    local.get 0
    i32.const 0
    call_indirect (type $unary64))
  (func (export "i64_store_load") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store
    local.get 0
    i64.load)
  (func (export "i64_store8_load_s") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store8
    local.get 0
    i64.load8_s)
  (func (export "i64_store8_load_u") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store8
    local.get 0
    i64.load8_u)
  (func (export "i64_store16_load_s") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store16
    local.get 0
    i64.load16_s)
  (func (export "i64_store16_load_u") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store16
    local.get 0
    i64.load16_u)
  (func (export "i64_store32_load_s") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store32
    local.get 0
    i64.load32_s)
  (func (export "i64_store32_load_u") (param i32 i64) (result i64)
    local.get 0
    local.get 1
    i64.store32
    local.get 0
    i64.load32_u)
  (func (export "i64_add") (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.add)
  (func (export "i64_sub") (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.sub)
  (func (export "i64_mul") (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.mul)
  (func (export "i64_const") (result i64)
    i64.const 4294967296)
  (func (export "i64_local") (param i64) (result i64) (local i64)
    local.get 0
    local.set 1
    local.get 1
    i64.const 3
    i64.mul)
  (func (export "i64_wrap") (result i64)
    i64.const 9223372036854775807
    i64.const 1
    i64.add)
  (func (export "i64_tee") (param i64) (result i64) (local i64)
    local.get 0
    local.tee 1
    drop
    local.get 1)
  (func (export "i64_mixed") (param i32 i64) (result i64)
    local.get 1
    local.get 0
    drop
    i64.const 1
    i64.add)
  (func (export "i64_and") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.and)
  (func (export "i64_or") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.or)
  (func (export "i64_xor") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.xor)
  (func (export "i64_shl") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.shl)
  (func (export "i64_shr_s") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.shr_s)
  (func (export "i64_shr_u") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.shr_u)
  (func (export "i64_rotl") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.rotl)
  (func (export "i64_rotr") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.rotr)
  (func (export "i64_eqz") (param i64) (result i32)
    local.get 0 i64.eqz)
  (func (export "i64_eq") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.eq)
  (func (export "i64_ne") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.ne)
  (func (export "i64_lt_s") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.lt_s)
  (func (export "i64_lt_u") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.lt_u)
  (func (export "i64_gt_s") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.gt_s)
  (func (export "i64_gt_u") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.gt_u)
  (func (export "i64_le_s") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.le_s)
  (func (export "i64_le_u") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.le_u)
  (func (export "i64_ge_s") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.ge_s)
  (func (export "i64_ge_u") (param i64 i64) (result i32)
    local.get 0 local.get 1 i64.ge_u)
  (func (export "i64_div_s") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.div_s)
  (func (export "i64_div_u") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.div_u)
  (func (export "i64_rem_s") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.rem_s)
  (func (export "i64_rem_u") (param i64 i64) (result i64)
    local.get 0 local.get 1 i64.rem_u)
)
