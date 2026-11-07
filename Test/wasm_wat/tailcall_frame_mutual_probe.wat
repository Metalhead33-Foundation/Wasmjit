;; Frame-form cross-function trampoline: four parameters force the staging-frame
;; ABI module-wide. The driver lays each callee's arguments out in
;; `TailCallState::frame` and copies the final result block back to the original
;; caller's frame, so the 1,000,000-link chain runs in constant native stack.
(module
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

  (func (export "even0") (result i32)
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    call $even)

  (func (export "run") (result i32)
    i32.const 1000000
    i32.const 0
    i32.const 0
    i32.const 0
    call $even))
