;; Frame-form function-reference calls: four parameters force the staging-frame
;; ABI. `run` enters through `call_ref`; each link uses `return_call_ref` on a
;; `ref.func` value. The frame driver must carry the arguments through
;; TailCallState::frame.
(module
  (type $t (func (param i32 i32 i32 i32) (result i32)))
  (elem declare func $even $odd)

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
      ref.func $odd
      return_call_ref $t
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
      ref.func $even
      return_call_ref $t
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    i32.const 0
    i32.const 0
    i32.const 0
    ref.func $even
    call_ref $t))
