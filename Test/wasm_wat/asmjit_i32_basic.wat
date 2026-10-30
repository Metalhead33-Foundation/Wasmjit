(module
  (memory 1 2)
  (global $g (export "g") (mut i32) (i32.const 7))
  (type $unary (func (param i32) (result i32)))
  (type $result_only (func (result i32)))
  (table 1 funcref)
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
  (func (export "choose") (param i32 i32 i32) (result i32) (local i32)
    local.get 0
    if
      local.get 1
      local.set 3
    else
      local.get 2
      local.set 3
    end
    local.get 3)
  (func (export "branch_if") (param i32) (result i32) (local i32)
    i32.const 10
    local.set 1
    block
      local.get 0
      br_if 0
      i32.const 20
      local.set 1
    end
    local.get 1)
  (func (export "branch") (result i32) (local i32)
    i32.const 10
    local.set 0
    block
      br 0
      i32.const 20
      local.set 0
    end
    local.get 0)
  (func (export "branch_to_function") (param i32) (result i32)
    local.get 0
    if
      i32.const 42
      br 1
    end
    i32.const 7)
  (func (export "branch_if_function") (param i32) (result i32)
    i32.const 42
    local.get 0
    br_if 0
    drop
    i32.const 7)
  (func (export "branch_table") (param i32) (result i32)
    block (result i32)
      block (result i32)
        i32.const 42
        local.get 0
        br_table 0 1 1
      end
      i32.const 1
      i32.add
    end)
  (func $unreachable_func (export "unreachable_trap") (result i32)
    unreachable)
  (func (export "call_unreachable") (result i32)
    call $unreachable_func)
  (func (export "unreachable_if") (param i32) (result i32)
    local.get 0
    if (result i32)
      unreachable
    else
      i32.const 7
    end)
  (func (export "call_forward") (param i32) (result i32)
    local.get 0
    call $double)
  (func $double (param i32) (result i32)
    local.get 0
    i32.const 2
    i32.mul)
  (func (export "call_tail") (param i32) (result i32)
    local.get 0
    return_call $double)
  (func (export "call_tail_indirect") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    return_call_indirect (type $unary))
  (func (export "store_load") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.store
    local.get 0
    i32.load)
  (func (export "store8_load8") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.store8
    local.get 0
    i32.load8_u)
  (func (export "load_oob") (param i32) (result i32)
    local.get 0
    i32.load)
  (func (export "mutual_recursion") (param i32) (result i32)
    local.get 0
    call $even
    i32.const 42)
  (func $even (param i32)
    local.get 0
    if
      local.get 0
      i32.const 1
      i32.sub
      call $odd
    end)
  (func $odd (param i32)
    local.get 0
    if
      local.get 0
      i32.const 1
      i32.sub
      call $even
    end)
  (func (export "extend8_s") (param i32) (result i32)
    local.get 0
    i32.extend8_s)
  (func (export "extend16_s") (param i32) (result i32)
    local.get 0
    i32.extend16_s)
  (func (export "loop_sum") (param i32) (result i32) (local i32)
    i32.const 0
    local.set 1
    block
      loop
        local.get 0
        i32.eqz
        br_if 1
        local.get 1
        local.get 0
        i32.add
        local.set 1
        local.get 0
        i32.const 1
        i32.sub
        local.set 0
        br 0
      end
    end
    local.get 1)
  (func (export "popcnt") (param i32) (result i32)
    local.get 0
    i32.popcnt)
  (func (export "clz") (param i32) (result i32)
    local.get 0
    i32.clz)
  (func (export "ctz") (param i32) (result i32)
    local.get 0
    i32.ctz)
  (func (export "div_s") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.div_s)
  (func (export "div_u") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.div_u)
  (func (export "rem_s") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.rem_s)
  (func (export "rem_u") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.rem_u)
  (func (export "store16_load16_s") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.store16 offset=2
    local.get 0
    i32.load16_s offset=2)
  (func (export "store16_load16_u") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.store16
    local.get 0
    i32.load16_u)
  (func (export "memory_size") (result i32)
    memory.size)
  (func (export "memory_grow") (param i32) (result i32)
    local.get 0
    memory.grow)
  (func (export "get_global") (result i32)
    global.get 0)
  (func (export "set_global") (param i32) (result i32)
    local.get 0
    global.set 0
    global.get 0)
  (func $table_double (type $unary)
    local.get 0
    i32.const 2
    i32.mul)
  (elem (i32.const 0) $table_double)
  (func (export "call_indirect") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    call_indirect (type $unary))
  (func (export "block_result") (param i32) (result i32)
    block (result i32)
      local.get 0
      i32.const 1
      i32.add
    end)
  (func (export "if_result") (param i32) (result i32)
    local.get 0
    if (result i32)
      i32.const 42
    else
      i32.const 7
    end)
  (func (export "br_result") (result i32)
    block (result i32)
      i32.const 42
      br 0
    end)
  (func (export "br_if_result") (param i32) (result i32)
    block (result i32)
      i32.const 42
      local.get 0
      br_if 0
      drop
      i32.const 7
    end)
  (func (export "loop_result") (result i32)
    loop (result i32)
      i32.const 42
    end)
  (func (export "typed_block_result") (param i32) (result i32)
    block (type $result_only)
      local.get 0
      i32.const 2
      i32.mul
    end)
  (func (export "local_roundtrip") (param i32) (result i32) (local i32)
    local.get 0
    local.set 1
    local.get 1
    i32.const 3
    i32.mul)
)
