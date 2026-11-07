;; Frame-form tail recursion with a small depth: LibJIT's frame tail path is a
;; frame call followed by a frame return, not the register trampoline.
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
    i32.const 100
    i32.const 0
    i32.const 0
    i32.const 0
    call $even))
