;; Frame-form indirect tail calls: four parameters force the staging-frame ABI.
;; even/odd alternate through a two-entry funcref table with return_call_indirect.
(module
  (type $t (func (param i32 i32 i32 i32) (result i32)))
  (table 2 funcref)
  (elem (i32.const 0) $even $odd)

  (func $even (param i32 i32 i32 i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 1
    else
      local.get 0
      i32.const 1
      i32.sub
      local.get 1
      local.get 2
      local.get 3
      i32.const 1
      return_call_indirect (type $t)
    end)

  (func $odd (param i32 i32 i32 i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 0
    else
      local.get 0
      i32.const 1
      i32.sub
      local.get 1
      local.get 2
      local.get 3
      i32.const 0
      return_call_indirect (type $t)
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    i32.const 0
    i32.const 0
    i32.const 0
    call $even))
