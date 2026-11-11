;; Frame-form tail recursion at 1,000,000 links: the stage-frame driver in
;; WasmBase carries the frame across every link, so the chain must not grow the
;; native stack (docs/API_ABI_STABILITY.md GAP-3, GR-7).
(module
  (type $t (func (param i32 i32 i32 i32) (result i32)))

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
      return_call $odd
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
      return_call $even
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    i32.const 0
    i32.const 0
    i32.const 0
    call $even))
